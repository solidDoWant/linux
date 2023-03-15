#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/platform_device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/circ_buf.h>
#include <linux/miscdevice.h>
#include <linux/gpio.h>
#include <dt-bindings/gpio/gpio.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#define M2DEV_MODEM_EM06    "4G-EM06"
#define M2DEV_MODEM_RM500Q  "5G-RM500Q"

#define MINIPCIEDEV_MODEM_EC25  "4G-EC25"
#define MINIPCIEDEV_LORA_RAK5146  "LORA-RAK5146"

#define LOG(x...)   pr_info("[customdev]: " x)

struct customdev_poweron_data {
	struct device *dev;
	char m2dev_name[32];
	char minipciedev_name[32];
	bool is_m2dev_support;
	bool is_ssd_support;
	bool is_minipciedev_support;
	struct gpio_desc *m2_reset_gpio;
	struct gpio_desc *m2_power_gpio;
	struct gpio_desc *m2_vbat_gpio;
	struct gpio_desc *m2_wakein_gpio;
	struct gpio_desc *m2_ssd_sel_gpio;
	struct gpio_desc *minipcie_reset_gpio;
	struct gpio_desc *minipcie_vbat_gpio;
	struct gpio_desc *minipcie_wakein_gpio;
};

static struct customdev_poweron_data *gpdata;
static struct class *customdev_class;
static int m2dev_status = 1;
static int minipciedev_status = 1;

static char *m2dev_support_list[] = {"SSD",
				 "5G-RM500Q",
				 "4G-EM06"
				 };

static char *minipciedev_support_list[] = {"4G-EC25",
				"LORA-RAK5146"
				 };

static int ec25_power(struct customdev_poweron_data *pdata, int on_off)
{
	if(on_off) {
		if (pdata->minipcie_vbat_gpio) {
			gpiod_direction_output(pdata->minipcie_vbat_gpio, 1);
			msleep(10);
		}

		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
			msleep(10);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 1);
			msleep(250);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
		}

	} else {
		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 1);
			msleep(10);
		}

		if (pdata->minipcie_vbat_gpio) {
			gpiod_direction_output(pdata->minipcie_vbat_gpio, 0);
		}
	}

	return 0;
}

static int rak5146_power(struct customdev_poweron_data *pdata, int on_off)
{
	if(on_off) {
		if (pdata->minipcie_vbat_gpio) {
			gpiod_direction_output(pdata->minipcie_vbat_gpio, 1);
			msleep(10);
		}

		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
			msleep(10);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 1);
			msleep(1);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
		}

	} else {
		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 1);
			msleep(10);
		}

		if (pdata->minipcie_vbat_gpio) {
			gpiod_direction_output(pdata->minipcie_vbat_gpio, 0);
		}
	}

	return 0;
}

static int ssd_power(struct customdev_poweron_data *pdata, int on_off)
{
	if(on_off) {
		if(pdata->m2_ssd_sel_gpio){
			gpiod_direction_output(pdata->m2_ssd_sel_gpio, 0);
			msleep(20);
		}

		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 1);
			msleep(10);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
			msleep(10);
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(50);
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
		}

	} else {
		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(10);
		}

		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 0);
		}

		if(pdata->m2_ssd_sel_gpio){
			gpiod_direction_output(pdata->m2_ssd_sel_gpio, 1);
			msleep(20);
		}
	}

	return 0;
}

static int em06_power(struct customdev_poweron_data *pdata, int on_off)
{
	if(on_off) {
		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 1);
			msleep(10);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
			msleep(10);
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(500);
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
		}

		if (pdata->m2_power_gpio) {
			gpiod_direction_output(pdata->m2_power_gpio, 0);
			msleep(30);
		}

	} else {
		if (pdata->m2_power_gpio) {
			gpiod_direction_output(pdata->m2_power_gpio, 1);
			msleep(10);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(30);
		}

		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 0);
		}

	}
	return 0;
}

static int rm500q_power(struct customdev_poweron_data *pdata, int on_off)
{
	if(on_off) {
		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 1);
			msleep(30);
		}

		if (pdata->m2_power_gpio) {
			gpiod_direction_output(pdata->m2_power_gpio, 0);
			msleep(30);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
			msleep(30);
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(400);
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
		}
	} else {
		if (pdata->m2_power_gpio) {
			gpiod_direction_output(pdata->m2_power_gpio, 1);
			msleep(8000);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(30);
		}

		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 0);
		}
	}

	return 0;
}

static int m2_device_poweron(int on_off)
{
	struct customdev_poweron_data *pdata = gpdata;
	int bret;
	LOG("m2 device %s power ops.\n", pdata->m2dev_name);

	if (pdata) {
		if(!strcmp(pdata->m2dev_name, M2DEV_MODEM_EM06))
			bret = em06_power(pdata, on_off);
		else if(!strcmp(pdata->m2dev_name, M2DEV_MODEM_RM500Q))
			bret = rm500q_power(pdata, on_off);
		else {
			bret = 0;
			LOG("m2 device name is not support\n");
		}
	}

	return bret;
}

static int ssd_poweron(int on_off)
{
	struct customdev_poweron_data *pdata = gpdata;
	int bret;
	LOG("ssd power on\n");

	if (pdata) {
		bret = ssd_power(pdata, on_off);
	}

	return bret;
}

static int minipcie_device_poweron(int on_off)
{
	struct customdev_poweron_data *pdata = gpdata;
	int bret;
	LOG("m2 device %s power ops.\n", pdata->minipciedev_name);

	if (pdata) {
		if(!strcmp(pdata->minipciedev_name, MINIPCIEDEV_MODEM_EC25))
			bret = ec25_power(pdata, on_off);
		else if(!strcmp(pdata->minipciedev_name, MINIPCIEDEV_LORA_RAK5146))
			bret = rak5146_power(pdata, on_off);
		else {
			bret = 0;
			LOG("minipcie device name is not support\n");
		}
	}

	return bret;
}

static ssize_t m2dev_onoff_store(struct class *cls,
				  struct class_attribute *attr,
				  const char *buf, size_t count)
{
	int new_state, ret;
	struct customdev_poweron_data *pdata = gpdata;

	ret = kstrtoint(buf, 10, &new_state);
	if (ret) {
		LOG("%s: kstrtoint error return %d\n", __func__, ret);
		return ret;
	}
	if (new_state == m2dev_status)
		return count;
	if (new_state == 1) {
		LOG("%s, c(%d), open m2dev.\n", __func__, new_state);
		if(pdata->is_m2dev_support) {
			if(pdata->is_ssd_support)
				ssd_poweron(1);
			else
				m2_device_poweron(1);
		}	
	} else if (new_state == 0) {
		LOG("%s, c(%d), close m2dev.\n", __func__, new_state);
		if(pdata->is_m2dev_support){
			if(pdata->is_ssd_support)
				ssd_poweron(0);
			else
				m2_device_poweron(0);
		}	
	} else {
		LOG("%s, invalid parameter.\n", __func__);
	}
	m2dev_status = new_state;
	return count;
}

static ssize_t m2dev_onoff_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	return sprintf(buf, "%d\n", m2dev_status);
}

static ssize_t minipciedev_onoff_store(struct class *cls,
				  struct class_attribute *attr,
				  const char *buf, size_t count)
{
	int new_state, ret;

	ret = kstrtoint(buf, 10, &new_state);
	if (ret) {
		LOG("%s: kstrtoint error return %d\n", __func__, ret);
		return ret;
	}
	if (new_state == minipciedev_status)
		return count;
	if (new_state == 1) {
		LOG("%s, c(%d), open m2dev.\n", __func__, new_state);
		minipcie_device_poweron(1);
	} else if (new_state == 0) {
		LOG("%s, c(%d), close m2dev.\n", __func__, new_state);
		minipcie_device_poweron(0);
	} else {
		LOG("%s, invalid parameter.\n", __func__);
	}
	minipciedev_status = new_state;
	return count;
}

static ssize_t minipciedev_onoff_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	return sprintf(buf, "%d\n", minipciedev_status);
}

static ssize_t m2dev_name_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	struct customdev_poweron_data *pdata = gpdata;
	return sprintf(buf, "%s\n", pdata->m2dev_name);
}

static ssize_t minipciedev_name_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	struct customdev_poweron_data *pdata = gpdata;

	return sprintf(buf, "%s\n", pdata->minipciedev_name);
}

static ssize_t m2dev_supportlist_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	struct customdev_poweron_data *pdata;

	ssize_t len = 0;
	int i = 0;
	int m2dev_list_len = sizeof(m2dev_support_list) / sizeof(m2dev_support_list[0]);

	pdata = gpdata;
	for(i=0; i<m2dev_list_len; i++){
		if (len >= PAGE_SIZE)
			break;
		
		len += snprintf(buf + len, PAGE_SIZE - len, "%s", m2dev_support_list[i]);
		
		if (len >= PAGE_SIZE)
			break;
		
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	}

	if (len >= PAGE_SIZE) {
		pr_warn_once("cpufreq transition table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}


	return len;
}

static ssize_t minipciedev_supportlist_show(struct class *cls,
				 struct class_attribute *attr,
				 char *buf)
{
	struct customdev_poweron_data *pdata;
	ssize_t len = 0;
	int i= 0;
	int minipciedev_list_len = sizeof(minipciedev_support_list) / sizeof(minipciedev_support_list[0]);

	pdata = gpdata;
	for(i=0; i<minipciedev_list_len; i++){
		if (len >= PAGE_SIZE)
			break;
		
		len += snprintf(buf + len, PAGE_SIZE - len, "%s", minipciedev_support_list[i]);
		
		if (len >= PAGE_SIZE)
			break;
		
		len += snprintf(buf + len, PAGE_SIZE - len, "\n");
	}

	if (len >= PAGE_SIZE) {
		pr_warn_once("cpufreq transition table exceeds PAGE_SIZE. Disabling\n");
		return -EFBIG;
	}

	return len;
}

static ssize_t m2dev_reset_store(struct class *cls,
				  struct class_attribute *attr,
				  const char *buf, size_t count)
{
	struct customdev_poweron_data *pdata = gpdata;

	printk("m2dev reset %s start\n", buf);
	if (!strncmp(buf, "RM500Q", strlen("RM500Q"))) {
		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
			msleep(30);
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(400);
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
		}
	} else if (!strncmp(buf, "EM06", strlen("EM06"))) {
		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
			msleep(30);
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
			msleep(400);
			gpiod_direction_output(pdata->m2_reset_gpio, 0);
		}
	} else {
		LOG("%s, reset device (%s) not support\n", __func__, buf);
	}

	return count;
}


static ssize_t minipciedev_reset_store(struct class *cls,
				  struct class_attribute *attr,
				  const char *buf, size_t count)
{
	struct customdev_poweron_data *pdata = gpdata;

	printk("minipcie reset %s start\n", buf);
	if (!strncmp(buf, "EC25", strlen("EC25"))) {
		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
			msleep(30);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 1);
			msleep(300);
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
		}
	} else {
		LOG("%s, reset device (%s) not support\n", __func__, buf);
	}
	
	return count;
}

static CLASS_ATTR_RW(m2dev_onoff);
static CLASS_ATTR_RO(m2dev_name);
static CLASS_ATTR_RW(minipciedev_onoff);
static CLASS_ATTR_RO(minipciedev_name);
static CLASS_ATTR_RO(m2dev_supportlist);
static CLASS_ATTR_RO(minipciedev_supportlist);
static CLASS_ATTR_WO(m2dev_reset);
static CLASS_ATTR_WO(minipciedev_reset);

int get_valid_devname(const char *strings, char* m2dev_name, char* minipciedev_name)
{
	char *m2dev_name_start, *minipciedev_name_start;
	char pstr[64];
	int i, m2dev_name_len, minipciedev_name_len;
	bool has_semicolon=false;
	int org_len = strlen(strings);

	memset(pstr, '\0', 64);
	memcpy(pstr, strings, org_len);

	if(org_len > 63)
		org_len = 63;

	for(i=0; i<org_len; i++){
		if(pstr[i] == ';'){
			has_semicolon=true;
			m2dev_name_len=i;
			break;
		}
	}

	if(!has_semicolon){
		if(org_len > 0){
			memcpy(m2dev_name, pstr, org_len);
			LOG("m2dev name is %s\n", m2dev_name);
		} else {
			LOG("no custom dev\n");
			return -1;
		}
	} else {
		minipciedev_name_len = org_len - m2dev_name_len -1;
		if(minipciedev_name_len > 0){
			minipciedev_name_start = &pstr[m2dev_name_len+1];
			memcpy(minipciedev_name, minipciedev_name_start, minipciedev_name_len);
			LOG("minipciedev name is %s\n", minipciedev_name);
		}

		if(m2dev_name_len > 0){
			m2dev_name_start = pstr;
			memcpy(m2dev_name, m2dev_name_start, m2dev_name_len);
			LOG("m2dev name is %s\n", m2dev_name);
		}

		if((minipciedev_name_len <= 0) && (m2dev_name_len <= 0)){
			LOG("no custom dev\n");
			return -1;
		}

	}
	return 0;
}

int is_support_m2dev(char* m2dev_name)
{
	int i;
	int m2dev_list_len = sizeof(m2dev_support_list) / sizeof(m2dev_support_list[0]);

	for(i=0; i<m2dev_list_len; i++){
		if(!strcmp(m2dev_name, m2dev_support_list[i])){
			return true;
		}
	}

	return false;
}

int is_support_ssd(char* m2dev_name)
{
	if(!strcmp(m2dev_name, "SSD")){
		return true;
	}
	return false;
}

int is_support_minipciedev(char* minipciedev_name)
{
	int i;
	int minipciedev_list_len = sizeof(minipciedev_support_list) / sizeof(minipciedev_support_list[0]);

	for(i=0; i<minipciedev_list_len; i++){
		if(!strcmp(minipciedev_name, minipciedev_support_list[i])){
			return true;
		}
	}

	return false;
}

static int custom_device_poweron_platdata_parse_dt(struct device *dev,
				   struct customdev_poweron_data *data)
{
	struct device_node *node = dev->of_node;
	const char *strings;
	int ret;

	if (!node)
		return -ENODEV;
	memset(data, 0, sizeof(*data));
	//初始化m2dev_name minipciedev_name
	memset(data->m2dev_name, '\0', 32);
	memset(data->minipciedev_name, '\0', 32);
	
	ret = of_property_read_string(node, "devname", &strings);
	if (ret) {
		LOG("get custom driver fdt for devname fail\n");
		return -1;
	} else {
		LOG("get custom driver fdt for devname:%s\n", strings);
	}

	if(get_valid_devname(strings, data->m2dev_name, data->minipciedev_name)<0){
		LOG("parse custom devname fail\n");
		return -1;
	}

	if(is_support_m2dev(data->m2dev_name)){
		//LOG("m2usbdev:%s is support\n", data->m2dev_name);
		data->is_m2dev_support=true;
		if(is_support_ssd(data->m2dev_name))
			data->is_ssd_support=true;
		else
			data->is_ssd_support=false;
	} else{
		data->is_m2dev_support=false;
	}

	if(!data->is_m2dev_support){
		LOG("m2dev:%s is not support\n", data->m2dev_name);
		memset(data->m2dev_name, '\0', 32);
	}

	if(is_support_minipciedev(data->minipciedev_name)){
		//LOG("minipciedev:%s is support\n", data->minipciedev_name);
		data->is_minipciedev_support=true;
	} else{
		LOG("minipciedev:%s is not support\n", data->minipciedev_name);
		memset(data->minipciedev_name, '\0', 32);
		data->is_minipciedev_support=false;
	}

	if((data->is_m2dev_support==false) && (data->is_minipciedev_support==false)){
		//LOG("m2dev and minipciedev is not support\n");
		return -1;
	}

	if(data->is_m2dev_support) {
		data->m2_vbat_gpio = devm_gpiod_get_optional(dev, "m2,vbat", GPIOD_OUT_LOW);
		if (IS_ERR(data->m2_vbat_gpio)) {
			ret = PTR_ERR(data->m2_vbat_gpio);
			dev_err(dev, "failed to request m2,vbat GPIO: %d\n", ret);
			return ret;
		}

		data->m2_power_gpio = devm_gpiod_get_optional(dev, "m2,power", GPIOD_OUT_HIGH);
		if (IS_ERR(data->m2_power_gpio)) {
			ret = PTR_ERR(data->m2_power_gpio);
			dev_err(dev, "failed to request m2,power GPIO: %d\n", ret);
			return ret;
		}
		data->m2_reset_gpio = devm_gpiod_get_optional(dev, "m2,reset", GPIOD_OUT_HIGH);
		if (IS_ERR(data->m2_reset_gpio)) {
			ret = PTR_ERR(data->m2_reset_gpio);
			dev_warn(dev, "failed to request m2,reset GPIO: %d\n", ret);
			return ret;
		}
		data->m2_wakein_gpio = devm_gpiod_get_optional(dev, "m2,wake-in", GPIOD_IN);
		if (IS_ERR(data->m2_wakein_gpio)) {
			ret = PTR_ERR(data->m2_wakein_gpio);
			dev_warn(dev, "failed to request m2,wakein GPIO: %d\n", ret);
		}
		data->m2_ssd_sel_gpio = devm_gpiod_get_optional(dev, "m2,ssdsel", GPIOD_OUT_HIGH);
		if (IS_ERR(data->m2_ssd_sel_gpio)) {
			ret = PTR_ERR(data->m2_ssd_sel_gpio);
			dev_warn(dev, "failed to request m2,ssdsel GPIO: %d\n", ret);
		}
	}

	if(data->is_minipciedev_support){
		data->minipcie_vbat_gpio = devm_gpiod_get_optional(dev, "minipcie,vbat", GPIOD_OUT_LOW);
		if (IS_ERR(data->minipcie_vbat_gpio)) {
			ret = PTR_ERR(data->minipcie_vbat_gpio);
			dev_err(dev, "failed to request minipcie,vbat GPIO: %d\n", ret);
			return ret;
		}
		data->minipcie_reset_gpio = devm_gpiod_get_optional(dev, "minipcie,reset", GPIOD_OUT_HIGH);
		if (IS_ERR(data->minipcie_reset_gpio)) {
			ret = PTR_ERR(data->minipcie_reset_gpio);
			dev_err(dev, "failed to request minipcie,power GPIO: %d\n", ret);
			return ret;
		}
		data->minipcie_wakein_gpio = devm_gpiod_get_optional(dev, "minipcie,wake-in", GPIOD_IN);
		if (IS_ERR(data->minipcie_wakein_gpio)) {
			ret = PTR_ERR(data->minipcie_wakein_gpio);
			dev_err(dev, "failed to request minipcie,wakein GPIO: %d\n", ret);
		}
	}

	return 0;
}

static int m2_power_on_thread(void *data)
{
	m2_device_poweron(1);
	return 0;
}

static int minipcie_power_on_thread(void *data)
{
	minipcie_device_poweron(1);
	return 0;
}

static int custom_device_poweron_probe(struct platform_device *pdev)
{
	struct customdev_poweron_data *pdata;
	struct task_struct *kthread_m2;
	struct task_struct *kthread_minipcie;
	int ret = -1;
	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;
	ret = custom_device_poweron_platdata_parse_dt(&pdev->dev, pdata);
	if (ret < 0) {
		LOG("%s: No custom device platform data specified\n", __func__);
		goto err;
	}
	gpdata = pdata;
	pdata->dev = &pdev->dev;

	if(pdata->is_m2dev_support){
		if(!pdata->is_ssd_support){
			kthread_m2 = kthread_run(m2_power_on_thread, NULL, "m2usb_power_on_thread");
			if (IS_ERR(kthread_m2)) {
				LOG("%s: create m2_power_on_thread failed.\n",  __func__);
				ret = PTR_ERR(kthread_m2);
				goto err;
			} 
		} else {
				ssd_poweron(1);
		}
	}

	if(pdata->is_minipciedev_support){
		kthread_minipcie = kthread_run(minipcie_power_on_thread, NULL, "minipcie_power_on_thread");
		if (IS_ERR(kthread_minipcie)) {
			LOG("%s: create minipcie_power_on_thread failed.\n",  __func__);
			ret = PTR_ERR(kthread_minipcie);
			goto err;
		}
	}
	
	return 0;
err:
	devm_kfree(&pdev->dev, pdata);
	gpdata = NULL;
	return ret;
}

static int custom__device_poweron_suspend(struct platform_device *pdev, pm_message_t state)
{
	return 0;
}

static int custom__device_poweron_resume(struct platform_device *pdev)
{
	return 0;
}

static int custom__device_poweron_remove(struct platform_device *pdev)
{
	struct customdev_poweron_data *pdata = gpdata;
	if(pdata->is_m2dev_support || pdata->is_ssd_support) {
		if (pdata->m2_power_gpio) {
			gpiod_direction_output(pdata->m2_power_gpio, 1);
		}

		if (pdata->m2_reset_gpio) {
			gpiod_direction_output(pdata->m2_reset_gpio, 1);
		}

		if (pdata->m2_vbat_gpio) {
			gpiod_direction_output(pdata->m2_vbat_gpio, 0);
		}

		if (pdata->m2_ssd_sel_gpio) {
			gpiod_direction_output(pdata->m2_ssd_sel_gpio, 1);
		}
	}

	if(pdata->is_minipciedev_support){
		if (pdata->minipcie_reset_gpio) {
			gpiod_direction_output(pdata->minipcie_reset_gpio, 0);
		}

		if (pdata->minipcie_vbat_gpio) {
			gpiod_direction_output(pdata->minipcie_vbat_gpio, 0);
		}
	}

	gpdata = NULL;
	return 0;
}

static const struct of_device_id custom__device_poweron_platdata_of_match[] = {
	{ .compatible = "custom-device-poweron-platdata" },
	{ }
};
MODULE_DEVICE_TABLE(of, custom__device_poweron_platdata_of_match);

static struct platform_driver custom_device_poweron_driver = {
	.probe		= custom_device_poweron_probe,
	.remove		= custom__device_poweron_remove,
	.suspend	= custom__device_poweron_suspend,
	.resume		= custom__device_poweron_resume,
	.driver	= {
		.name	= "custom_-device-poweron-platdata",
		.of_match_table = of_match_ptr(custom__device_poweron_platdata_of_match),
	},
};

static int __init custom_device_poweron_init(void)
{
	int ret;

	customdev_class = class_create(THIS_MODULE, "customdev");
	ret =  class_create_file(customdev_class, &class_attr_m2dev_onoff);
	if (ret)
		LOG("Fail to create class m2dev_onoff.\n");
	ret =  class_create_file(customdev_class, &class_attr_m2dev_name);
	if (ret)
		LOG("Fail to create class m2dev_name.\n");
	
	ret =  class_create_file(customdev_class, &class_attr_minipciedev_onoff);
	if (ret)
		LOG("Fail to create class minipciedev_onoff.\n");
	ret =  class_create_file(customdev_class, &class_attr_minipciedev_name);
	if (ret)
		LOG("Fail to create class minipciedev_name.\n");
	ret =  class_create_file(customdev_class, &class_attr_m2dev_supportlist);
	if (ret)
		LOG("Fail to create class minipciedev_name.\n");
	ret =  class_create_file(customdev_class, &class_attr_minipciedev_supportlist);
	if (ret)
		LOG("Fail to create class minipciedev_name.\n");
	ret =  class_create_file(customdev_class, &class_attr_m2dev_reset);
	if (ret)
		LOG("Fail to create class m2dev_reset.\n");
	ret =  class_create_file(customdev_class, &class_attr_minipciedev_reset);
	if (ret)
		LOG("Fail to create class minipciedev_reset.\n");
	return platform_driver_register(&custom_device_poweron_driver);
}

static void __exit custom_device_poweron_exit(void)
{
	platform_driver_unregister(&custom_device_poweron_driver);
}

subsys_initcall(custom_device_poweron_init);
module_exit(custom_device_poweron_exit);

MODULE_AUTHOR("zhangping <zhangping@focalcrest.com>");
MODULE_AUTHOR("zhangping <zhangping@focalcrest.com>");
MODULE_DESCRIPTION("custom device poweron driver");
MODULE_LICENSE("GPL");
