// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * pwm-fan.c - Hwmon driver for fans connected to PWM lines.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *
 * Author: Kamil Debski <k.debski@samsung.com>
 */

#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/regulator/consumer.h>
#include <linux/sysfs.h>
#include <linux/thermal.h>
#include <linux/timer.h>

#define MAX_PWM 255
#define DEV_MAX_NUMS 255

struct __thermal_zone {
	int passive_delay;
	int polling_delay;
	int slope;
	int offset;

	/* trip data */
	int ntrips;
	struct thermal_trip *trips;

	/* cooling binding data */
	int num_tbps;
	struct __thermal_bind_params *tbps;

	/* sensor interface */
	void *sensor_data;
	const struct thermal_zone_of_device_ops *ops;
};

struct pwm_fan_ctx {
	struct mutex lock;
	struct pwm_device *pwm;
	struct regulator *reg_en;

	int irq;
	atomic_t pulses;
	unsigned int rpm;
	u8 pulses_per_revolution;
	ktime_t sample_start;
	struct timer_list rpm_timer;

	unsigned int pwm_value;
	unsigned int pwm_fan_state;
	unsigned int pwm_fan_max_state;
	unsigned int *pwm_fan_cooling_levels;
	struct thermal_cooling_device *cdev;
};

struct device_fan {
	int index;
	struct pwm_fan_ctx *fan_ctx[DEV_MAX_NUMS];
}device_fan;

/* This handler assumes self resetting edge triggered interrupt. */
static irqreturn_t pulse_handler(int irq, void *dev_id)
{
	struct pwm_fan_ctx *ctx = dev_id;
	atomic_inc(&ctx->pulses);
	return IRQ_HANDLED;
}

static int fan_get_temp(int *temp)
{
	struct __thermal_zone *data;
	struct thermal_zone_device *dz;

	dz = thermal_zone_get_zone_by_name("soc-thermal");
	if (!dz) {
		printk(KERN_INFO "%s %s line is %d dz is null\r\n", __FILE__, __FUNCTION__, __LINE__);
	}
	data = (struct __thermal_zone *)(dz->devdata);
	if (!data->ops || !data->ops->get_temp) {
		return -EINVAL;
	}
	return data->ops->get_temp(data->sensor_data, temp);
}

static int __set_pwm(struct pwm_fan_ctx *ctx, unsigned long pwm)
{
	unsigned long period;
	int ret = 0;
	struct pwm_state state = { };

	mutex_lock(&ctx->lock);
	if (ctx->pwm_value == pwm)
		goto exit_set_pwm_err;

	pwm_init_state(ctx->pwm, &state);
	period = ctx->pwm->args.period;
	state.duty_cycle = DIV_ROUND_UP(pwm * (period - 1), MAX_PWM);
	state.enabled = pwm ? true : false;

	ret = pwm_apply_state(ctx->pwm, &state);
	if (!ret)
		ctx->pwm_value = pwm;

exit_set_pwm_err:
	mutex_unlock(&ctx->lock);
	return ret;
}

static void pwm_fan_update_state(struct pwm_fan_ctx *ctx, unsigned long pwm)
{
	int i;

	for (i = 0; i < ctx->pwm_fan_max_state; ++i)
		if (pwm < ctx->pwm_fan_cooling_levels[i + 1])
			break;

	ctx->pwm_fan_state = i;
}

static ssize_t pwm_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	unsigned long pwm;
	int ret;

	if (kstrtoul(buf, 10, &pwm) || pwm > MAX_PWM)
		return -EINVAL;

	ret = __set_pwm(ctx, pwm);
	if (ret)
		return ret;

	pwm_fan_update_state(ctx, pwm);
	return count;
}

static ssize_t pwm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->pwm_value);
}

static ssize_t rpm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", ctx->rpm);
}

static SENSOR_DEVICE_ATTR_RW(fan_pwm, pwm, 0);
static SENSOR_DEVICE_ATTR_RO(fan_rpm, rpm, 0);

static struct attribute *pwm_fan_attrs[] = {
	&sensor_dev_attr_fan_pwm.dev_attr.attr,
	&sensor_dev_attr_fan_rpm.dev_attr.attr,
	NULL,
};

static umode_t pwm_fan_attrs_visible(struct kobject *kobj, struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);

	/* Hide fan_input in case no interrupt is available  */
	if (n == 1 && ctx->irq <= 0)
		return 0;

	return a->mode;
}

static const struct attribute_group pwm_fan_group = {
	.attrs = pwm_fan_attrs,
	.is_visible = pwm_fan_attrs_visible,
};

static const struct attribute_group *pwm_fan_groups[] = {
	&pwm_fan_group,
	NULL,
};

/* thermal cooling device callbacks */
static int pwm_fan_get_max_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->pwm_fan_max_state;

	return 0;
}

static int pwm_fan_get_cur_state(struct thermal_cooling_device *cdev, unsigned long *state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;

	if (!ctx)
		return -EINVAL;

	*state = ctx->pwm_fan_state;

	return 0;
}

static int pwm_fan_set_cur_state(struct thermal_cooling_device *cdev, unsigned long state)
{
	struct pwm_fan_ctx *ctx = cdev->devdata;
	int ret;

	if (!ctx || (state > ctx->pwm_fan_max_state))
		return -EINVAL;

	if (state == ctx->pwm_fan_state)
		return 0;

	ret = __set_pwm(ctx, ctx->pwm_fan_cooling_levels[state]);
	if (ret) {
		dev_err(&cdev->device, "Cannot set pwm!\n");
		return ret;
	}

	ctx->pwm_fan_state = state;

	return ret;
}

static int pwm_fan_monitor_fan(int temperature, unsigned int *pwm_val)
{
    if(temperature > 0 && temperature <= 20000) {
        *pwm_val = 0;
    }
    else if(temperature > 20000 && temperature <= 30000) {
        *pwm_val = 50;
    }
    else if(temperature > 30000 && temperature <= 40000) {
        *pwm_val = 100;
    }
    else if(temperature > 40000 && temperature <= 50000) {
        *pwm_val = 200;
    }
    else if(temperature > 50000) {
        *pwm_val = 255;
    }

    return 0;
}

static const struct thermal_cooling_device_ops pwm_fan_cooling_ops = {
	.get_max_state = pwm_fan_get_max_state,
	.get_cur_state = pwm_fan_get_cur_state,
	.set_cur_state = pwm_fan_set_cur_state,
};

static int pwm_fan_of_get_cooling_data(struct device *dev,
				       struct pwm_fan_ctx *ctx)
{
	struct device_node *np = dev->of_node;
	int num, i, ret;

	if (!of_find_property(np, "cooling-levels", NULL))
		return 0;

	ret = of_property_count_u32_elems(np, "cooling-levels");
	if (ret <= 0) {
		dev_err(dev, "Wrong data!\n");
		return ret ? : -EINVAL;
	}

	num = ret;
	ctx->pwm_fan_cooling_levels = devm_kcalloc(dev, num, sizeof(u32),
						   GFP_KERNEL);
	if (!ctx->pwm_fan_cooling_levels)
		return -ENOMEM;

	ret = of_property_read_u32_array(np, "cooling-levels",
					 ctx->pwm_fan_cooling_levels, num);
	if (ret) {
		dev_err(dev, "Property 'cooling-levels' cannot be read!\n");
		return ret;
	}

	for (i = 0; i < num; i++) {
		if (ctx->pwm_fan_cooling_levels[i] > MAX_PWM) {
			dev_err(dev, "PWM fan state[%d]:%d > %d\n", i,
				ctx->pwm_fan_cooling_levels[i], MAX_PWM);
			return -EINVAL;
		}
	}

	ctx->pwm_fan_max_state = num - 1;

	return 0;
}

static void pwm_fan_regulator_disable(void *data)
{
	regulator_disable(data);
}

static void pwm_fan_pwm_disable(void *__ctx)
{
	struct pwm_fan_ctx *ctx = __ctx;
	pwm_disable(ctx->pwm);
	del_timer_sync(&ctx->rpm_timer);
}

static void sample_timer(struct timer_list *t)
{
	struct pwm_fan_ctx *ctx = from_timer(ctx, t, rpm_timer);
	unsigned int delta = ktime_ms_delta(ktime_get(), ctx->sample_start);
	int pulses, temp, ret;
	unsigned int pwm_val;

	if (delta) {
		pulses = atomic_read(&ctx->pulses);
		atomic_sub(pulses, &ctx->pulses);
		ctx->rpm = (unsigned int)(pulses * 1000 * 60) /
			(ctx->pulses_per_revolution * delta);
		ctx->sample_start = ktime_get();
	}
	if (fan_get_temp(&temp)) {
		printk(KERN_INFO "%s %s line is %d get temp fail\r\n", __FILE__, __FUNCTION__, __LINE__);
		goto out;
	}

	pwm_fan_monitor_fan(temp, &pwm_val);
	if (!(device_fan.fan_ctx[0])) {
		printk(KERN_INFO "%s %s line is %d no find fan_ctx\r\n", __FILE__, __FUNCTION__, __LINE__);
		goto out;
	}

	ret = __set_pwm(device_fan.fan_ctx[0], pwm_val);
	if (ret) {
		printk(KERN_INFO "%s %s line is %d Cannot set pwm!\r\n", __FILE__, __FUNCTION__, __LINE__);
	}

out:
	mod_timer(&ctx->rpm_timer, jiffies + HZ);
}

static int pwm_fan_probe(struct platform_device *pdev)
{
	struct thermal_cooling_device *cdev;
	struct device *dev = &pdev->dev;
	struct pwm_fan_ctx *ctx;
	struct device *hwmon;
	int ret;
	struct pwm_state state = { };
	u32 ppr = 2;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	if (device_fan.index >= 0 && device_fan.index < DEV_MAX_NUMS) {
		device_fan.fan_ctx[device_fan.index++] = ctx;
	} else {
		printk(KERN_INFO "%s %s line is %d device max nums is 255\r\n", __FILE__, __FUNCTION__, __LINE__);
		return -EINVAL;
	}
	mutex_init(&ctx->lock);

	ctx->pwm = devm_of_pwm_get(dev, dev->of_node, NULL);
	if (IS_ERR(ctx->pwm))
		return dev_err_probe(dev, PTR_ERR(ctx->pwm), "Could not get PWM\n");

	platform_set_drvdata(pdev, ctx);

	ctx->irq = platform_get_irq_optional(pdev, 0);
	if (ctx->irq == -EPROBE_DEFER)
		return ctx->irq;

	ctx->reg_en = devm_regulator_get_optional(dev, "fan");
	if (IS_ERR(ctx->reg_en)) {
		if (PTR_ERR(ctx->reg_en) != -ENODEV)
			return PTR_ERR(ctx->reg_en);

		ctx->reg_en = NULL;
	} else {
		ret = regulator_enable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to enable fan supply: %d\n", ret);
			return ret;
		}
		ret = devm_add_action_or_reset(dev, pwm_fan_regulator_disable,
					       ctx->reg_en);
		if (ret)
			return ret;
	}

	ctx->pwm_value = MAX_PWM;
	pwm_init_state(ctx->pwm, &state);
	/*
	 * __set_pwm assumes that MAX_PWM * (period - 1) fits into an unsigned
	 * long. Check this here to prevent the fan running at a too low
	 * frequency.
	 */
	if (state.period > ULONG_MAX / MAX_PWM + 1) {
		dev_err(dev, "Configured period too big\n");
		return -EINVAL;
	}

	/* Set duty cycle to maximum allowed and enable PWM output */
	state.duty_cycle = ctx->pwm->args.period - 1;
	state.enabled = true;

	ret = pwm_apply_state(ctx->pwm, &state);
	if (ret) {
		dev_err(dev, "Failed to configure PWM: %d\n", ret);
		return ret;
	}

	timer_setup(&ctx->rpm_timer, sample_timer, 0);
	ret = devm_add_action_or_reset(dev, pwm_fan_pwm_disable, ctx);
	if (ret)
		return ret;

	of_property_read_u32(dev->of_node, "pulses-per-revolution", &ppr);
	ctx->pulses_per_revolution = ppr;
	if (!ctx->pulses_per_revolution) {
		dev_err(dev, "pulses-per-revolution can't be zero.\n");
		return -EINVAL;
	}

	if (ctx->irq > 0) {
		ret = devm_request_irq(dev, ctx->irq, pulse_handler, 0,
				       pdev->name, ctx);
		if (ret) {
			dev_err(dev, "Failed to request interrupt: %d\n", ret);
			return ret;
		}
		ctx->sample_start = ktime_get();
		mod_timer(&ctx->rpm_timer, jiffies + HZ);
	}

	hwmon = devm_hwmon_device_register_with_groups(dev, "pwmfan",
						       ctx, pwm_fan_groups);
	if (IS_ERR(hwmon)) {
		dev_err(dev, "Failed to register hwmon device\n");
		return PTR_ERR(hwmon);
	}
	ret = pwm_fan_of_get_cooling_data(dev, ctx);
	if (ret)
		return ret;

	ctx->pwm_fan_state = ctx->pwm_fan_max_state;
	if (IS_ENABLED(CONFIG_THERMAL)) {
		cdev = devm_thermal_of_cooling_device_register(dev,
			dev->of_node, "pwm-fan", ctx, &pwm_fan_cooling_ops);
		if (IS_ERR(cdev)) {
			ret = PTR_ERR(cdev);
			dev_err(dev,
				"Failed to register pwm-fan as cooling device: %d\n",
				ret);
			return ret;
		}
		ctx->cdev = cdev;
		thermal_cdev_update(cdev);
	}

	return 0;
}

static int pwm_fan_disable(struct device *dev)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	struct pwm_args args;
	int ret;

	pwm_get_args(ctx->pwm, &args);

	if (ctx->pwm_value) {
		ret = pwm_config(ctx->pwm, 0, args.period);
		if (ret < 0)
			return ret;

		pwm_disable(ctx->pwm);
	}

	if (ctx->reg_en) {
		ret = regulator_disable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to disable fan supply: %d\n", ret);
			return ret;
		}
	}

	return 0;
}

static void pwm_fan_shutdown(struct platform_device *pdev)
{
	pwm_fan_disable(&pdev->dev);
}

#ifdef CONFIG_PM_SLEEP
static int pwm_fan_suspend(struct device *dev)
{
	return pwm_fan_disable(dev);
}

static int pwm_fan_resume(struct device *dev)
{
	struct pwm_fan_ctx *ctx = dev_get_drvdata(dev);
	struct pwm_args pargs;
	unsigned long duty;
	int ret;

	if (ctx->reg_en) {
		ret = regulator_enable(ctx->reg_en);
		if (ret) {
			dev_err(dev, "Failed to enable fan supply: %d\n", ret);
			return ret;
		}
	}

	if (ctx->pwm_value == 0)
		return 0;

	pwm_get_args(ctx->pwm, &pargs);
	duty = DIV_ROUND_UP_ULL(ctx->pwm_value * (pargs.period - 1), MAX_PWM);
	ret = pwm_config(ctx->pwm, duty, pargs.period);
	if (ret)
		return ret;
	return pwm_enable(ctx->pwm);
}
#endif

static SIMPLE_DEV_PM_OPS(pwm_fan_pm, pwm_fan_suspend, pwm_fan_resume);

static const struct of_device_id of_pwm_fan_match[] = {
	{ .compatible = "pwm-fan", },
	{},
};
MODULE_DEVICE_TABLE(of, of_pwm_fan_match);

static struct platform_driver pwm_fan_driver = {
	.probe		= pwm_fan_probe,
	.shutdown	= pwm_fan_shutdown,
	.driver	= {
		.name		= "pwm-fan",
		.pm		= &pwm_fan_pm,
		.of_match_table	= of_pwm_fan_match,
	},
};

module_platform_driver(pwm_fan_driver);

MODULE_AUTHOR("Kamil Debski <k.debski@samsung.com>");
MODULE_ALIAS("platform:pwm-fan");
MODULE_DESCRIPTION("PWM FAN driver");
MODULE_LICENSE("GPL");
