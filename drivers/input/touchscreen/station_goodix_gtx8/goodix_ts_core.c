 /*
  * Goodix Touchscreen Driver
  * Core layer of touchdriver architecture.
  *
  * Copyright (C) 2015 - 2016 Goodix, Inc.
  * Authors:  Yulong Cai <caiyulong@goodix.com>
  *
  * This program is free software; you can redistribute it and/or modify
  * it under the terms of the GNU General Public License as published by
  * the Free Software Foundation; either version 2 of the License, or
  * (at your option) any later version.
  *
  * This program is distributed in the hope that it will be a reference
  * to you, when you are integrating the GOODiX's CTP IC into your system,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  * General Public License for more details.
  *
  */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>
#include <linux/completion.h>
#include <linux/debugfs.h>
#include <linux/of_irq.h>
#ifdef CONFIG_FB
#include <linux/notifier.h>
#include <linux/msm_drm_notify.h>
#include <linux/fb.h>
#endif

#include "goodix_ts_core.h"

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 38)
#include <linux/input/mt.h>
#define INPUT_TYPE_B_PROTOCOL
#endif

#define GOOIDX_INPUT_PHYS	"goodix_ts/input0"
#define PINCTRL_STATE_ACTIVE    "pmx_ts_active"
#define PINCTRL_STATE_SUSPEND   "pmx_ts_suspend"

#define Station_TP_RST 0x35

extern int ec_i2c_set_gpio(u8 gpio, u8 value);
extern int goodix_i2c_init(void);
//extern int goodix_tools_init(void);
extern int goodix_fwu_module_init(void);
extern void goodix_i2c_exit(void);
//extern void goodix_tools_exit(void);
extern void goodix_fwu_module_exit(void);
int goodix_station_ts_success = 0;
int goodix_ts_fw_update = 0;
static void goodix_station_suspend_work(struct work_struct *work);
static void goodix_station_resume_work(struct work_struct *work);

extern int goodix_start_cfg_bin(struct goodix_ts_core *ts_core);

extern atomic_t g_notify_fod;
extern void TP_call_FOD(int x, int y);

struct goodix_module goodix_modules;
struct goodix_ts_core *gts_core_data = NULL;

/**
 * __do_register_ext_module - register external module
 * to register into touch core modules structure
 */
static void  __do_register_ext_module(struct work_struct *work)
{
	struct goodix_ext_module *module =
			container_of(work, struct goodix_ext_module, work);
	struct goodix_ext_module *ext_module;
	struct list_head *insert_point = &goodix_modules.head;

	ts_info("__do_register_ext_module IN, goodix_modules.core_exit:%d", goodix_modules.core_exit);
	ts_info("register external module %s",module->name);
	
	/* waitting for core layer */
	if (!wait_for_completion_timeout(&goodix_modules.core_comp, 25 * HZ)) {
		ts_err("Module [%s] timeout", module->name);
		return;
	}

	/* driver probe failed */
	if (goodix_modules.core_exit) {
		ts_err("Can't register ext_module, core exit");
		return;
	}

	ts_info("start register ext_module");

	/* prority level *must* be set */
	if (module->priority == EXTMOD_PRIO_RESERVED) {
		ts_err("Priority of module [%s] needs to be set",
				module->name);
		return;
	}

	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (ext_module == module) {
				ts_info("Module [%s] already exists",
						module->name);
				mutex_unlock(&goodix_modules.mutex);
				return;
			}
		}

		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			/* small value of priority have
			 * higher priority level*/
			if (ext_module->priority >= module->priority) {
				insert_point = &ext_module->list;
				break;
			}
		} /* else module will be inserted
		 to goodix_modules->head */
	}

	if (module->funcs && module->funcs->init) {
		if (module->funcs->init(goodix_modules.core_data,
					module) < 0) {
			ts_err("Module [%s] init error",
					module->name ? module->name : " ");
			mutex_unlock(&goodix_modules.mutex);
			return;
		}
	}

	list_add(&module->list, insert_point->prev);
	goodix_modules.count++;
	mutex_unlock(&goodix_modules.mutex);

	ts_info("Module [%s] registered,priority:%u",
			module->name,
			module->priority);
	return;
}

/**
 * station_goodix_register_ext_module - interface for external module
 * to register into touch core modules structure
 *
 * @module: pointer to external module to be register
 * return: 0 ok, <0 failed
 */
int station_goodix_register_ext_module(struct goodix_ext_module *module)
{
	if (!module)
		return -EINVAL;
        
	if (!goodix_modules.initilized) {
	        ts_info("goodix_modules.initilized is false");
		goodix_modules.initilized = true;
		goodix_modules.core_exit = true;
		INIT_LIST_HEAD(&goodix_modules.head);
		mutex_init(&goodix_modules.mutex);
		init_completion(&goodix_modules.core_comp);
	}

/*	if (goodix_modules.core_exit) {
		ts_err("Can't register ext_module, core exit");
		return -EFAULT;
	}
*/
	ts_info("station_goodix_register_ext_module IN");

	INIT_WORK(&module->work, __do_register_ext_module);
	schedule_work(&module->work);

	ts_info("station_goodix_register_ext_module OUT");


	return 0;
}
EXPORT_SYMBOL_GPL(station_goodix_register_ext_module);

/**
 * station_goodix_unregister_ext_module - interface for external module
 * to unregister external modules
 *
 * @module: pointer to external module
 * return: 0 ok, <0 failed
 */
int station_goodix_unregister_ext_module(struct goodix_ext_module *module)
{
	struct goodix_ext_module *ext_module;
	bool found = false;

	if (!module)
		return -EINVAL;

	if (!goodix_modules.initilized)
		return -EINVAL;

	if (!goodix_modules.core_data)
		return -ENODEV;

	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (ext_module == module) {
				found = true;
				break;
			}
		}
	} else {
		mutex_unlock(&goodix_modules.mutex);
		return -EFAULT;
	}

	if (!found) {
		ts_err("Module [%s] never registed",
				module->name);
		mutex_unlock(&goodix_modules.mutex);
		return -EFAULT;
	}

	list_del(&module->list);
	mutex_unlock(&goodix_modules.mutex);

	if (module->funcs && module->funcs->exit)
		module->funcs->exit(goodix_modules.core_data, module);
	goodix_modules.count--;

	ts_info("Moudle [%s] unregistered",
			module->name ? module->name : " ");
	return 0;
}
EXPORT_SYMBOL_GPL(station_goodix_unregister_ext_module);

static void goodix_ext_sysfs_release(struct kobject *kobj)
{
	ts_info("Kobject released!");
}

#define to_ext_module(kobj)	container_of(kobj,\
				struct goodix_ext_module, kobj)
#define to_ext_attr(attr)	container_of(attr,\
				struct goodix_ext_attribute, attr)

static ssize_t goodix_ext_sysfs_show(struct kobject *kobj,
		struct attribute *attr, char *buf)
{
	struct goodix_ext_module *module = to_ext_module(kobj);
	struct goodix_ext_attribute *ext_attr = to_ext_attr(attr);

	if (ext_attr->show)
		return ext_attr->show(module, buf);

	return -EIO;
}

static ssize_t goodix_ext_sysfs_store(struct kobject *kobj,
		struct attribute *attr, const char *buf, size_t count)
{
	struct goodix_ext_module *module = to_ext_module(kobj);
	struct goodix_ext_attribute *ext_attr = to_ext_attr(attr);

	if (ext_attr->store)
		return ext_attr->store(module, buf, count);

	return -EIO;
}

static const struct sysfs_ops goodix_ext_ops = {
	.show = goodix_ext_sysfs_show,
	.store = goodix_ext_sysfs_store
};

static struct kobj_type goodix_ext_ktype = {
	.release = goodix_ext_sysfs_release,
	.sysfs_ops = &goodix_ext_ops,
};

struct kobj_type *station_goodix_get_default_ktype(void)
{
	return &goodix_ext_ktype;
}
EXPORT_SYMBOL_GPL(station_goodix_get_default_ktype);

struct kobject *station_station_goodix_get_default_kobj(void)
{
	struct kobject *kobj = NULL;

	if (goodix_modules.core_data &&
			goodix_modules.core_data->pdev)
		kobj = &goodix_modules.core_data->pdev->dev.kobj;
	return kobj;
}
EXPORT_SYMBOL_GPL(station_station_goodix_get_default_kobj);

/* debug fs */
struct debugfs_buf {
	struct debugfs_blob_wrapper buf;
	int pos;
	struct dentry *dentry;
} goodix_dbg;

void station_goodix_msg_printf(const char *fmt, ...)
{
	va_list args;
	int r;

	if (goodix_dbg.pos < goodix_dbg.buf.size) {
		va_start(args, fmt);
		r = vscnprintf(goodix_dbg.buf.data + goodix_dbg.pos,
			 goodix_dbg.buf.size - 1, fmt, args);
		goodix_dbg.pos += r;
		va_end(args);
	}
}
EXPORT_SYMBOL_GPL(station_goodix_msg_printf);

static int goodix_debugfs_init(void)
{
	struct dentry *r_b;
	goodix_dbg.buf.size = PAGE_SIZE;
	goodix_dbg.pos = 0;
	goodix_dbg.buf.data = kzalloc(goodix_dbg.buf.size, GFP_KERNEL);
	if (goodix_dbg.buf.data == NULL) {
		pr_err("Debugfs init failed\n");
		goto exit;
	}
	r_b = debugfs_create_blob("station_goodix_ts", 0644, NULL, &goodix_dbg.buf);
	if (!r_b) {
		pr_err("Debugfs create failed\n");
		return -ENOENT;
	}
	goodix_dbg.dentry = r_b;

exit:
	return 0;
}

static void goodix_debugfs_exit(void)
{
	debugfs_remove(goodix_dbg.dentry);
	goodix_dbg.dentry = NULL;
	pr_info("Debugfs module exit\n");
}

/* show external module infomation */
static ssize_t goodix_ts_extmod_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct goodix_ext_module *module;
	size_t offset = 0;
	int r;

	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(module, &goodix_modules.head, list) {
			r = snprintf(&buf[offset], PAGE_SIZE,
					"priority:%u module:%s\n",
					module->priority, module->name);
			if (r < 0) {
				mutex_unlock(&goodix_modules.mutex);
				return -EINVAL;
			}
			offset += r;
		}
	}

	mutex_unlock(&goodix_modules.mutex);
	return offset;
}

/* show driver infomation */
static ssize_t goodix_ts_driver_info_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "DriverVersion:%s\n",
			GOODIX_DRIVER_VERSION);
}

/* show chip infoamtion */
static ssize_t goodix_ts_chip_info_show(struct device  *dev,
		struct device_attribute *attr, char *buf)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	struct goodix_ts_version chip_ver;
	int r, cnt = 0;

	if (goodix_station_ts_success == 0) {
		ts_info("goodix station touch init = %d, goodix_ts_probe fail", goodix_station_ts_success);
		cnt += snprintf(buf, PAGE_SIZE, "version_wrong\n");
		return	cnt;
	}

	cnt += snprintf(buf, PAGE_SIZE,
			"TouchDeviceName:%s\n", ts_dev->name);

	if (ts_dev->hw_ops->read_version) {
		r = ts_dev->hw_ops->read_version(ts_dev, &chip_ver);
		if (!r && chip_ver.valid) {
			cnt += snprintf(&buf[cnt], PAGE_SIZE,
					"PID:%s\nVID:%02x.%02x.%02x.%02x\nSensorID:%02x\n",
					chip_ver.pid, chip_ver.vid[0],
					chip_ver.vid[1], chip_ver.vid[2],
					chip_ver.vid[3], chip_ver.sensor_id);
		} else {
			cnt += snprintf(&buf[cnt], PAGE_SIZE,
					"PID:00\nVID:00.00.00.00\nSensorID:00\n");
		}
	}

	return cnt;
}

/* show goodix station touch status */
static ssize_t goodix_ts_init_show(struct device  *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", goodix_station_ts_success);
}

/* show chip configuration data */
static ssize_t goodix_ts_config_data_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	struct goodix_ts_config *ncfg = ts_dev->normal_cfg;
	u8 *data;
	int i, r, offset = 0;

	if (ncfg && ncfg->initialized && ncfg->length < PAGE_SIZE) {
		data = kmalloc(ncfg->length, GFP_KERNEL);
		if (!data)
			return -ENOMEM;

		r = ts_dev->hw_ops->read(ts_dev, ncfg->reg_base,
				&data[0], ncfg->length);
		if (r < 0) {
			kfree(data);
			return -EINVAL;
		}

		for (i = 0; i < ncfg->length; i++) {
			if (i != 0 && i % 20 == 0)
				buf[offset++] = '\n';
			offset += snprintf(&buf[offset], PAGE_SIZE - offset,
					"%02x ", data[i]);
		}
		buf[offset++] = '\n';
		buf[offset++] = '\0';
		kfree(data);
		return offset;
	}

	return -EINVAL;
}

/* reset chip */
static ssize_t goodix_ts_reset_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t count)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int en;

	if (sscanf(buf, "%d", &en) != 1)
		return -EINVAL;

	if (en != 1)
		return -EINVAL;

	if (ts_dev->hw_ops->reset)
		ts_dev->hw_ops->reset(ts_dev);
	return count;

}

static ssize_t goodix_ts_read_cfg_show(struct device *dev,
				struct device_attribute *attr,
						char *buf)
{
	struct goodix_ts_core *core_data =
				dev_get_drvdata(dev);
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int ret, i, offset;
	char *cfg_buf;

	cfg_buf = kzalloc(4096, GFP_KERNEL);
	disable_irq(core_data->irq);
	if (ts_dev->hw_ops->read_config)
		ret = ts_dev->hw_ops->read_config(ts_dev, cfg_buf, 0);
	else
		ret = -EINVAL;
	enable_irq(core_data->irq);

	offset = 0;
	if (ret > 0) {
		for (i = 0; i < ret; i++) {
			if (i != 0 && i % 20 == 0)
				buf[offset++] = '\n';
			offset += snprintf(&buf[offset], 4096 - offset,
					"%02x ", cfg_buf[i]);
		}

	}
	kfree(cfg_buf);
	return ret;
}

static int goodix_ts_convert_0x_data(const u8 *buf,
				int buf_size,
				unsigned char *out_buf,
				int *out_buf_len)
{
	int i, m_size = 0;
	int temp_index = 0;

	for (i = 0; i < buf_size; i++) {
		if (buf[i] == 'x' || buf[i] == 'X')
			m_size++;
	}
	ts_info("***m_size:%d", m_size);

	if (m_size <= 1) {
		ts_err("cfg file ERROR, valid data count:%d\n", m_size);
		return -EINVAL;
	}
	*out_buf_len = m_size;

	for (i = 0; i < buf_size; i++) {
		if (buf[i] == 'x' || buf[i] == 'X') {
			if (temp_index >= m_size) {
				ts_err("exchange cfg data error, overflow, temp_index:%d,m_size:%d\n",
						temp_index, m_size);
				return -EINVAL;
			}
			if (buf[i + 1] >= '0' && buf[i + 1] <= '9')
				out_buf[temp_index] = (buf[i + 1] - '0') << 4;
			else if (buf[i + 1] >= 'a' && buf[i + 1] <= 'f')
				out_buf[temp_index] = (buf[i + 1] - 'a' + 10) << 4;
			else if (buf[i + 1] >= 'A' && buf[i + 1] <= 'F')
				out_buf[temp_index] = (buf[i + 1] - 'A' + 10) << 4;

			if (buf[i + 2] >= '0' && buf[i + 2] <= '9')
				out_buf[temp_index] += (buf[i + 2] - '0');
			else if (buf[i + 2] >= 'a' && buf[i + 2] <= 'f')
				out_buf[temp_index] += (buf[i + 2] - 'a' + 10);
			else if (buf[i + 2] >= 'A' && buf[i + 2] <= 'F')
				out_buf[temp_index] += (buf[i + 2] - 'A' + 10);

			temp_index++;
		}
	}
	return 0;
}



static ssize_t goodix_ts_send_cfg_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf,
				size_t count)
{
	struct goodix_ts_core *core_data =
				dev_get_drvdata(dev);
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int en, r;
	const struct firmware *cfg_img;
	struct goodix_ts_config *config = NULL;

	ts_info("******IN");

	if (sscanf(buf, "%d", &en) != 1)
		return -EINVAL;

	if (en != 1)
		return -EINVAL;

	ts_info("en:%d", en);

	disable_irq(core_data->irq);

	/*request configuration*/
	r = request_firmware(&cfg_img, GOODIX_DEFAULT_CFG_NAME, dev);
	if (r < 0) {
		ts_err("cfg file [%s] not available,errno:%d", GOODIX_DEFAULT_CFG_NAME, r);
		goto exit;
	} else
		ts_info("cfg file [%s] is ready", GOODIX_DEFAULT_CFG_NAME);

	config = kzalloc(sizeof(struct goodix_ts_config), GFP_KERNEL);
	if (config == NULL) {
		ts_err("Memory allco err");
		goto exit;
	}

	/*parse cfg data*/
	if (goodix_ts_convert_0x_data(cfg_img->data, cfg_img->size,
				config->data, &config->length)) {
		ts_err("convert config data FAILED");
		goto exit;
	}

	config->reg_base = ts_dev->reg.cfg_addr;
	mutex_init(&config->lock);
	config->initialized = true;

	if (ts_dev->hw_ops->send_config)
		ts_dev->hw_ops->send_config(ts_dev, config);

exit:
	enable_irq(core_data->irq);

	if (config) {
		kfree(config);
		config = NULL;
	}

	if (cfg_img) {
		release_firmware(cfg_img);
		cfg_img = NULL;
	}

	ts_info("******OUT");
	return count;
}

/* show irq infomation */
static ssize_t goodix_ts_irq_info_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
	struct irq_desc *desc;
	size_t offset = 0;
	int r;

	r = snprintf(&buf[offset], PAGE_SIZE, "irq:%u\n",
			core_data->irq);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "state:%s\n",
		atomic_read(&core_data->irq_enabled) ?
		"enabled" : "disabled");
	if (r < 0)
		return -EINVAL;

	desc = irq_to_desc(core_data->irq);
	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "disable-depth:%d\n",
		desc->depth);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset, "trigger-count:%zu\n",
		core_data->irq_trig_cnt);
	if (r < 0)
		return -EINVAL;

	offset += r;
	r = snprintf(&buf[offset], PAGE_SIZE - offset,
			"echo 0/1 > irq_info to disable/enable irq");
	if (r < 0)
		return -EINVAL;

	offset += r;
	return offset;
}

/* enable/disable irq */
static ssize_t goodix_ts_irq_info_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t count)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
	int en;

	if (sscanf(buf, "%d", &en) != 1)
		return -EINVAL;

	station_goodix_ts_irq_enable(core_data, en);
	return count;
}

// power on/off
static ssize_t goodix_ts_power_on_off(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t count)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
		
	int en, result;

	if (sscanf(buf, "%d", &en) != 1)
		return -EINVAL;

	station_goodix_ts_irq_enable(core_data, en);
	
	if (core_data->power_on == 1 && en == 0) {
		result = goodix_ts_power_off(core_data);
	}
	if (core_data->power_on == 0 && en == 1)
		result = goodix_ts_power_on(core_data);
	ts_info("power operation status %d",result);
	
	return count;

}

static ssize_t goodix_ts_power_state(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
    struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);

    return snprintf(buf, PAGE_SIZE, "%d\n",core_data->power_on);
}

/* fw update */
static ssize_t goodix_ts_fw_update_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf,
		size_t count)
{
	int en;

	if (sscanf(buf, "%d", &en) != 1)
		return -EINVAL;

	if (en) {
		goodix_ts_fw_update = 1;
		goodix_fwu_module_init();
	}

	return count;
}

static DEVICE_ATTR(extmod_info, S_IRUGO, goodix_ts_extmod_show, NULL);
static DEVICE_ATTR(driver_info, S_IRUGO, goodix_ts_driver_info_show, NULL);
static DEVICE_ATTR(chip_info, S_IRUGO, goodix_ts_chip_info_show, NULL);
static DEVICE_ATTR(goodix_init, S_IRUGO, goodix_ts_init_show, NULL);
static DEVICE_ATTR(config_data, S_IRUGO, goodix_ts_config_data_show, NULL);
static DEVICE_ATTR(reset, S_IWUSR | S_IWGRP, NULL, goodix_ts_reset_store);
static DEVICE_ATTR(send_cfg, S_IWUSR | S_IWGRP, NULL, goodix_ts_send_cfg_store);
static DEVICE_ATTR(read_cfg, S_IRUGO, goodix_ts_read_cfg_show, NULL);
static DEVICE_ATTR(irq_info, S_IRUGO | S_IWUSR | S_IWGRP,
		goodix_ts_irq_info_show, goodix_ts_irq_info_store);

static DEVICE_ATTR(power_on_off, S_IWUSR | S_IWGRP, NULL, goodix_ts_power_on_off);
static DEVICE_ATTR(power_state, S_IRUGO, goodix_ts_power_state, NULL);
static DEVICE_ATTR(fw_update, S_IWUSR | S_IWGRP, NULL, goodix_ts_fw_update_store);

static struct attribute *sysfs_attrs[] = {
	&dev_attr_extmod_info.attr,
	&dev_attr_driver_info.attr,
	&dev_attr_chip_info.attr,
	&dev_attr_goodix_init.attr,
	&dev_attr_config_data.attr,
	&dev_attr_reset.attr,
	&dev_attr_send_cfg.attr,
	&dev_attr_read_cfg.attr,
	&dev_attr_irq_info.attr,
	&dev_attr_power_on_off.attr,
	&dev_attr_power_state.attr,
	&dev_attr_fw_update.attr,
	NULL,
};

static const struct attribute_group sysfs_group = {
	.attrs = sysfs_attrs,
};

int goodix_ts_sysfs_init(struct goodix_ts_core *core_data)
{
	int ret = 0;
	ret = sysfs_create_group(&core_data->pdev->dev.kobj, &sysfs_group);
	if (ret) {
		ts_info("[EX]: sysfs_create_group() failed!!");
		return -1;
	} else {
		ts_info("[EX]: sysfs_create_group() succeeded!!");
		return 0;
	}
}

static void goodix_ts_sysfs_exit(struct goodix_ts_core *core_data)
{
	sysfs_remove_group(&core_data->pdev->dev.kobj, &sysfs_group);
}

/* event notifier */
static BLOCKING_NOTIFIER_HEAD(ts_notifier_list);
/**
 * goodix_ts_register_client - register a client notifier
 * @nb: notifier block to callback on events
 *  see enum ts_notify_event in goodix_ts_core.h
 */
int station_goodix_ts_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&ts_notifier_list, nb);
}
EXPORT_SYMBOL(station_goodix_ts_register_notifier);

/**
 * goodix_ts_unregister_client - unregister a client notifier
 * @nb: notifier block to callback on events
 *	see enum ts_notify_event in goodix_ts_core.h
 */
int station_goodix_ts_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&ts_notifier_list, nb);
}
EXPORT_SYMBOL(station_goodix_ts_unregister_notifier);

/**
 * fb_notifier_call_chain - notify clients of fb_events
 *	see enum ts_notify_event in goodix_ts_core.h
 */
int station_goodix_ts_blocking_notify(enum ts_notify_event evt, void *v)
{
	return blocking_notifier_call_chain(&ts_notifier_list,
			(unsigned long)evt, v);
}
EXPORT_SYMBOL_GPL(station_goodix_ts_blocking_notify);



/**
 * goodix_ts_input_report - report touch event to input subsystem
 *
 * @dev: input device pointer
 * @touch_data: touch data pointer
 * return: 0 ok, <0 failed
 */
static int goodix_ts_input_report(struct input_dev *dev,
		struct goodix_touch_data *touch_data)
{
	struct goodix_ts_coords *coords = &touch_data->coords[0];
	struct goodix_ts_core *core_data = input_get_drvdata(dev);
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	unsigned int touch_num = touch_data->touch_num;
	static u32 pre_fin = 0x0000;
	static u8 pre_key = 0x00;
	int i, id;

	/*first touch down and last touch up condition*/
	if (touch_num != 0 && pre_fin == 0x0000) {
		/*first touch down event*/
		input_report_key(dev, BTN_TOUCH, 1);
		if (touch_num == 1 && !touch_data->pen_down && atomic_read(&g_notify_fod)) {
			TP_call_FOD(touch_data->coords[0].x, touch_data->coords[0].y);
		}
	} else if (touch_num == 0 && pre_fin != 0x0000) {
		/*no finger exist*/
		input_report_key(dev, BTN_TOUCH, 0);
	if (atomic_read(&g_notify_fod))
		TP_call_FOD(0, 0);
	}

	/*report key, include tp's key and pen's key */
	if (unlikely(touch_data->have_key)) {
		for (i = 0; i < ts_bdata->panel_max_key; i++) {
			input_report_key(dev, ts_bdata->panel_key_map[i],
							touch_data->key_value & (1 << i));
		}
		pre_key = touch_data->key_value;
		/*ts_info("$$$$$$pre_key:0x%02x",pre_key);*/
	} else if (pre_key != 0x00) {
		/*ts_info("******no key, by pre_key is not ZERO! pre_key:0x%02x", pre_key);*/
		for (i = 0; i < ts_bdata->panel_max_key; i++) {
			if (pre_key & (1 << i)) {
				input_report_key(dev, ts_bdata->panel_key_map[i], 0);
				pre_key &= ~(1 << i);
				/*ts_info("******report i:%d, key:%d leave", i, ts_bdata->panel_key_map[i]);*/
			}
		}
		/*ts_info("******after, pre_key:0x%02x", pre_key);*/
	}

#if 1
	/*protocol B*/

	/*report pen*/
	if (touch_num >= 1 && touch_data->pen_down) {
		touch_num -= 1;

		input_mt_slot(dev, ts_bdata->panel_max_id * 2);
		input_mt_report_slot_state(dev, MT_TOOL_PEN, true);

		input_report_abs(dev, ABS_MT_POSITION_X, touch_data->pen_coords[0].x);
		input_report_abs(dev, ABS_MT_POSITION_Y, touch_data->pen_coords[0].y);
		input_report_abs(dev, ABS_MT_TOUCH_MAJOR, touch_data->pen_coords[0].w);
		input_report_abs(dev, ABS_MT_PRESSURE, touch_data->pen_coords[0].p);

		pre_fin |= 1 << 20;
		/*ts_info("!!!!!!report pen  DOWN,%d,%d,%d",
				touch_data->pen_coords[0].x,
				touch_data->pen_coords[0].y,
				touch_data->pen_coords[0].p);*/
	} else {
		if (pre_fin & (1 << 20)) {
			input_mt_slot(dev, ts_bdata->panel_max_id * 2);
			input_mt_report_slot_state(dev, MT_TOOL_PEN, false);
			pre_fin &= ~(1 << 20);
			/*ts_info("!!!!!!report pen LEAVE");*/
		}
	}

	/*report finger*/
	id = coords->id;
	for (i = 0; i < ts_bdata->panel_max_id * 2; i++) {
		if (touch_num && i == id) { /* this is a valid touch down event */
			input_mt_slot(dev, id);
			input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);

			input_report_abs(dev, ABS_MT_POSITION_X, coords->x);
			input_report_abs(dev, ABS_MT_POSITION_Y, coords->y);
			input_report_abs(dev, ABS_MT_TOUCH_MAJOR, coords->w);
			input_report_abs(dev, ABS_MT_PRESSURE, coords->p);

			pre_fin |= 1 << i;
			id = (++coords)->id;
		} else {
			if (pre_fin & (1 << i)) {/* release touch */
				input_mt_slot(dev, i);
				input_mt_report_slot_state(dev, MT_TOOL_FINGER, false);
				pre_fin &= ~(1 << i);
				/*ts_info("report leave:%d", i);*/
			}
		}
	}
#endif

#if 0
	/*report pen use protocl A*/
	if (touch_data->pen_down) {
		/*input_report_key(dev, BTN_TOOL_PEN, 1);*/
		/*input_report_key(dev, BTN_TOUCH, 1);*/

		input_report_abs(dev, ABS_MT_POSITION_X, touch_data->pen_coords[0].x);
		input_report_abs(dev, ABS_MT_POSITION_Y, touch_data->pen_coords[0].y);

		input_report_abs(dev, ABS_MT_PRESSURE, touch_data->pen_coords[0].p);
		input_report_abs(dev, ABS_MT_TOUCH_MAJOR, touch_data->pen_coords[0].w);
		/*input_report_abs(dev, ABS_MT_TRACKING_ID, touch_data->pen_coords[0].id);*/
		input_report_abs(dev, ABS_MT_TOOL_TYPE, 1);

		input_mt_sync(dev);
	} else {
		if (pre_fin & (1 << 10) && touch_num == 0) {
			/*input_report_key(dev, BTN_TOOL_PEN, 0);*/
			/*input_report_key(dev, BTN_TOUCH, 0);*/

			pre_fin &= ~(1 << 10);
		}
	}

	/* report abs */
	id = coords->id;
	for (i = 0; i < ts_bdata->panel_max_id; i++) {
		if (touch_num && i == id) { /* this is a valid touch down event */

			/*input_report_key(dev, BTN_TOUCH, 1);*/
			/*input_report_abs(dev, ABS_MT_TRACKING_ID, id);*/

			input_report_abs(dev, ABS_MT_POSITION_X, coords->x);
			input_report_abs(dev, ABS_MT_POSITION_Y, coords->y);
			input_report_abs(dev, ABS_MT_TOUCH_MAJOR, coords->w);
			input_report_abs(dev, ABS_MT_PRESSURE, coords->p);
			input_mt_sync(dev);

			pre_fin |= 1 << i;
			id = (++coords)->id;
		} else {
			if (pre_fin & (1 << i)) {
				/*input_mt_sync(dev);*/

				pre_fin &= ~(1 << i);
			}
		}
	}
#endif

	input_sync(dev);
	return 0;
}


/**
 * goodix_ts_threadirq_func - Bottom half of interrupt
 * This functions is excuted in thread context,
 * sleep in this function is permit.
 *
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static irqreturn_t goodix_ts_threadirq_func(int irq, void *data)
{
	struct goodix_ts_core *core_data = data;
	struct goodix_ts_device *ts_dev =  core_data->ts_dev;
	struct goodix_ext_module *ext_module;
	struct goodix_ts_event *ts_event = &core_data->ts_event;
	int r;

	core_data->irq_trig_cnt++;
	/* inform external module */
	mutex_lock(&goodix_modules.mutex);
	list_for_each_entry(ext_module, &goodix_modules.head, list) {
		if (!ext_module->funcs->irq_event)
			continue;
		r = ext_module->funcs->irq_event(core_data, ext_module);
		if (r == EVT_CANCEL_IRQEVT) {
			mutex_unlock(&goodix_modules.mutex);
			return IRQ_HANDLED;
		}
	}
	mutex_unlock(&goodix_modules.mutex);

	/* read touch data from touch device */
	r = ts_dev->hw_ops->event_handler(ts_dev, ts_event);
	if (likely(r >= 0)) {
		if (ts_event->event_type == EVENT_TOUCH) {
			/* report touch */
			goodix_ts_input_report(core_data->input_dev,
					&ts_event->event_data.touch_data);
		}
	}
	return IRQ_HANDLED;
}

/**
 * goodix_ts_init_irq - Requset interrput line from system
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_irq_setup(struct goodix_ts_core *core_data)
{
	const struct goodix_ts_board_data *ts_bdata =
			board_data(core_data);
	int r;

	/* if ts_bdata-> irq is invalid */
	if (ts_bdata->irq <= 0) {
		core_data->irq = gpio_to_irq(ts_bdata->irq_gpio);
	} else {
		core_data->irq = ts_bdata->irq;
	}

	ts_info("IRQ:%u,flags:%d", core_data->irq, (int)ts_bdata->irq_flags);
	r = devm_request_threaded_irq(&core_data->pdev->dev,
			core_data->irq, NULL,
			goodix_ts_threadirq_func,
			ts_bdata->irq_flags | IRQF_ONESHOT,
			GOODIX_CORE_DRIVER_NAME,
			core_data);
	if (r < 0)
		ts_err("Failed to requeset threaded irq:%d", r);
	else
		atomic_set(&core_data->irq_enabled, 1);

	return r;
}

/**
 * station_goodix_ts_irq_enable - Enable/Disable a irq
 * @core_data: pointer to touch core data
 * enable: enable or disable irq
 * return: 0 ok, <0 failed
 */
int station_goodix_ts_irq_enable(struct goodix_ts_core *core_data,
			bool enable)
{
	if (enable) {
		if (!atomic_cmpxchg(&core_data->irq_enabled, 0, 1)) {
			enable_irq(core_data->irq);
			ts_debug("Irq enabled");
		}
	} else {
		if (atomic_cmpxchg(&core_data->irq_enabled, 1, 0)) {
			disable_irq(core_data->irq);
			ts_debug("Irq disabled");
		}
	}

	return 0;
}
EXPORT_SYMBOL(station_goodix_ts_irq_enable);
/**
 * goodix_ts_power_init - Get regulator for touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_power_init(struct goodix_ts_core *core_data)
{
	struct device *dev = NULL;
	struct goodix_ts_board_data *ts_bdata;
	int r = 0;

	ts_info("Power init");
	/* dev:i2c client device or spi slave device*/
	dev =  core_data->ts_dev->dev;
	ts_bdata = board_data(core_data);

	if (ts_bdata->avdd_name) {
		core_data->avdd = devm_regulator_get(dev,
				 ts_bdata->avdd_name);
		if (IS_ERR_OR_NULL(core_data->avdd)) {
			r = PTR_ERR(core_data->avdd);
			ts_err("Failed to get regulator avdd:%d", r);
			core_data->avdd = NULL;
			return r;
		}
	} else {
		ts_info("Avdd name is NULL");
	}

	return r;
}

/**
 * goodix_ts_power_on - Turn on power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_power_on(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata =
			board_data(core_data);
	int r;

	ts_info("Device power on");
	if (core_data->power_on)
		return 0;

	if (core_data->avdd) {
		r = regulator_enable(core_data->avdd);
		if (!r) {
			ts_info("regulator enable SUCCESS");
			if (ts_bdata->power_on_delay_us)
				usleep_range(ts_bdata->power_on_delay_us,
						ts_bdata->power_on_delay_us);
		} else {
			ts_err("Failed to enable analog power:%d", r);
			return r;
		}
	}

	core_data->power_on = 1;
	return 0;
}

/**
 * goodix_ts_power_off - Turn off power to the touch device
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_power_off(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata =
			board_data(core_data);
	int r;

	ts_info("Device power off");
	if (!core_data->power_on)
		return 0;

	if (core_data->avdd) {
		r = regulator_disable(core_data->avdd);
		if (!r) {
			ts_info("regulator disable SUCCESS");
			if (ts_bdata->power_off_delay_us)
				usleep_range(ts_bdata->power_off_delay_us,
						ts_bdata->power_off_delay_us);
		} else {
			ts_err("Failed to disable analog power:%d", r);
			return r;
		}
	}

	core_data->power_on = 0;
	return 0;
}

//#ifdef CONFIG_PINCTRL
/**
 * goodix_ts_pinctrl_init - Get pinctrl handler and pinctrl_state
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
/*static int goodix_ts_pinctrl_init(struct goodix_ts_core *core_data)
{
	int r = 0;
        ts_info("goodix_ts_pinctrl_init start");
	//get pinctrl handler from of node
	core_data->pinctrl = devm_pinctrl_get(core_data->ts_dev->dev);
	if (IS_ERR_OR_NULL(core_data->pinctrl)) {
		ts_err("Failed to get pinctrl handler");
		return PTR_ERR(core_data->pinctrl);
	}

	//active state 
	core_data->pin_sta_active = pinctrl_lookup_state(core_data->pinctrl,
				PINCTRL_STATE_ACTIVE);
	if (IS_ERR_OR_NULL(core_data->pin_sta_active)) {
		r = PTR_ERR(core_data->pin_sta_active);
		ts_err("Failed to get pinctrl state:%s, r:%d",
				PINCTRL_STATE_ACTIVE, r);
		goto exit_pinctrl_put;
	}

	//suspend state
	core_data->pin_sta_suspend = pinctrl_lookup_state(core_data->pinctrl,
				PINCTRL_STATE_SUSPEND);
	if (IS_ERR_OR_NULL(core_data->pin_sta_suspend)) {
		r = PTR_ERR(core_data->pin_sta_suspend);
		ts_err("Failed to get pinctrl state:%s, r:%d",
				PINCTRL_STATE_SUSPEND, r);
		goto exit_pinctrl_put;
	}

	ts_info("goodix_ts_pinctrl_init complete");
	return 0;
exit_pinctrl_put:
	devm_pinctrl_put(core_data->pinctrl);
	core_data->pinctrl = NULL;
	core_data->pin_sta_active = NULL;
	core_data->pin_sta_suspend = NULL; 
	return r;
}
#endif*/

/**
 * goodix_ts_gpio_setup - Request gpio resources from GPIO subsysten
 *	reset_gpio and irq_gpio number are obtained from goodix_ts_device
 *  which created in hardware layer driver. e.g.goodix_xx_i2c.c
 *	A goodix_ts_device should set those two fileds to right value
 *	before registed to touch core driver.
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
static int goodix_ts_gpio_setup(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata =
			 board_data(core_data);
	int r = 0;

	ts_info("GPIO setup, irq-gpio:%d", ts_bdata->irq_gpio);
	/*
	 * after kenerl3.13, gpio_ api is deprecated, new
	 * driver should use gpiod_ api.
	 */
	
	/*if (gpio_is_valid(ts_bdata->reset_gpio)) {
		r = devm_gpio_request_one(&core_data->pdev->dev,
				ts_bdata->reset_gpio,
				GPIOF_OUT_INIT_HIGH,
				"ts_reset_gpio");
		if (r < 0) {
			ts_err("Failed to request reset gpio, r:%d", r);
			return r;
		}
	}*/

	ec_i2c_set_gpio(Station_TP_RST, 1);
	msleep(100);

	r = devm_gpio_request_one(&core_data->pdev->dev,
			ts_bdata->irq_gpio,
			GPIOF_IN,
			"station_ts_irq_gpio");
	if (r < 0) {
		ts_err("Failed to request irq gpio, r:%d", r);
		return r;
	}

	return 0;
}

/**
 * goodix_input_set_params - set input parameters
 */
static void goodix_ts_set_input_params(struct input_dev *input_dev,
		struct goodix_ts_board_data *ts_bdata)
{
	int i;

	if (ts_bdata->swap_axis)
		swap(ts_bdata->panel_max_x, ts_bdata->panel_max_y);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X,
			0, ts_bdata->panel_max_x, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y,
			0, ts_bdata->panel_max_y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR,
			0, ts_bdata->panel_max_w, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE,
			0, ts_bdata->panel_max_p, 0, 0);

	if (ts_bdata->panel_max_key) {
		for (i = 0; i < ts_bdata->panel_max_key; i++)
			input_set_capability(input_dev, EV_KEY,
					ts_bdata->panel_key_map[i]);
	}
}

/**
 * goodix_ts_input_dev_config - Requset and config a input device
 *  then register it to input sybsystem.
 *  NOTE that some hardware layer may provide a input device
 *  (ts_dev->input_dev not NULL).
 * @core_data: pointer to touch core data
 * return: 0 ok, <0 failed
 */
int goodix_ts_input_dev_config(struct goodix_ts_core *core_data)
{
	struct goodix_ts_board_data *ts_bdata = board_data(core_data);
	struct input_dev *input_dev = NULL;
	int r;

	input_dev = devm_input_allocate_device(&core_data->pdev->dev);
	if (!input_dev) {
		ts_err("Failed to allocated input device");
		return -ENOMEM;
	}

	core_data->input_dev = input_dev;
	input_set_drvdata(input_dev, core_data);

	input_dev->name = GOODIX_CORE_DRIVER_NAME;
	input_dev->phys = GOOIDX_INPUT_PHYS;
	input_dev->id.product = 0xDEAD;
	input_dev->id.vendor = 0xBEEF;
	input_dev->id.version = 10427;

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(BTN_TOUCH, input_dev->keybit);
	__set_bit(BTN_TOOL_FINGER, input_dev->keybit);
	__set_bit(BTN_TOOL_PEN, input_dev->keybit);

#ifdef INPUT_PROP_DIRECT
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#endif

	/* set input parameters */
	goodix_ts_set_input_params(input_dev, ts_bdata);

#ifdef INPUT_TYPE_B_PROTOCOL
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 7, 0)
	/*input_mt_init_slots(input_dev, ts_bdata->panel_max_id,
			INPUT_MT_DIRECT);*/
	input_mt_init_slots(input_dev,
			ts_bdata->panel_max_id * 2 + 1,
			INPUT_MT_DIRECT);
#else
	/*input_mt_init_slots(input_dev, ts_bdata->panel_max_id);*/
	input_mt_init_slots(input_dev,
			ts_bdata->panel_max_id * 2 + 1);
#endif
#endif

	input_set_capability(input_dev, EV_KEY, KEY_POWER);

	r = input_register_device(input_dev);
	if (r < 0) {
		ts_err("Unable to register input device");
		return r;
	}

	return 0;
}

/**
 * goodix_ts_hw_init - Hardware initilize
 *  poweron - hardware reset - sendconfig
 * @core_data: pointer to touch core data
 * return: 0 intilize ok, <0 failed
 */
int goodix_ts_hw_init(struct goodix_ts_core *core_data)
{
	const struct goodix_ts_hw_ops *hw_ops =
		ts_hw_ops(core_data);
	int r;
	
	/* reset touch device */
	if (hw_ops->reset) {
		r = hw_ops->reset(core_data->ts_dev);
		if (r < 0)
			goto exit;
	}

	/* init */
	if (hw_ops->init) {
		r = hw_ops->init(core_data->ts_dev);
		if (r < 0)
			goto exit;
	}

exit:
	/* if bus communication error occured then
	 * exit driver binding, other errors will
	 * be ignored */
	if (r != -EBUS)
		r = 0;
	return r;
}

/**
 * goodix_ts_esd_work - check hardware status and recovery
 *  the hardware if needed.
 */
static void goodix_ts_esd_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct goodix_ts_esd *ts_esd = container_of(dwork,
			struct goodix_ts_esd, esd_work);
	struct goodix_ts_core *core = container_of(ts_esd,
			struct goodix_ts_core, ts_esd);
	const struct goodix_ts_hw_ops *hw_ops = ts_hw_ops(core);
	int r = 0;
	u8 data = GOODIX_ESD_TICK_WRITE_DATA;

	if (ts_esd->esd_on == false)
		return;

	if (hw_ops->check_hw)
		r = hw_ops->check_hw(core->ts_dev);
	if (r < 0) {
		goodix_ts_power_off(core);
		goodix_ts_power_on(core);
		if (hw_ops->reset)
			hw_ops->reset(core->ts_dev);

		/*init static esd*/
		if (core->ts_dev->ic_type == IC_TYPE_NANJING) {
			r = hw_ops->write(core->ts_dev,
					0x8043, &data, 1);
			if (r < 0)
				ts_err("nanjing esd reset, init static esd FAILED, i2c wirte ERROR");
		}

		/*init dynamic esd*/
		r = hw_ops->write_trans(core->ts_dev,
				core->ts_dev->reg.esd,
				&data, 1);
		if (r < 0)
			ts_err("esd reset, init dynamic esd FAILED, i2c write ERROR");

	} else {
		/*init dynamic esd*/
		r = hw_ops->write_trans(core->ts_dev,
				core->ts_dev->reg.esd,
				&data, 1);
		if (r < 0)
			ts_err("esd init watch dog FAILED, i2c write ERROR");
	}

	/*mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on)
		schedule_delayed_work(&ts_esd->esd_work, 2 * HZ);
	mutex_unlock(&ts_esd->esd_mutex);*/
}

/**
 * goodix_ts_esd_on - turn on esd protection
 */
static void goodix_ts_esd_on(struct goodix_ts_core *core)
{
	struct goodix_ts_esd *ts_esd = &core->ts_esd;

	if(core->ts_dev->reg.esd == 0)
		return;

	mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on == false) {
		ts_esd->esd_on = true;
		schedule_delayed_work(&ts_esd->esd_work, 2 * HZ);
		mutex_unlock(&ts_esd->esd_mutex);
		ts_info("Esd on");
		return;
	}
	mutex_unlock(&ts_esd->esd_mutex);
}

/**
 * goodix_ts_esd_off - turn off esd protection
 */
static void goodix_ts_esd_off(struct goodix_ts_core *core)
{
	struct goodix_ts_esd *ts_esd = &core->ts_esd;

	mutex_lock(&ts_esd->esd_mutex);
	if (ts_esd->esd_on == true) {
		ts_esd->esd_on = false;
		cancel_delayed_work_sync(&ts_esd->esd_work);
		mutex_unlock(&ts_esd->esd_mutex);
		ts_info("Esd off");
		return;
	}
	mutex_unlock(&ts_esd->esd_mutex);
}

/**
 * goodix_esd_notifier_callback - notification callback
 *  under certain condition, we need to turn off/on the esd
 *  protector, we use kernel notify call chain to achieve this.
 *
 *  for example: before firmware update we need to turn off the
 *  esd protector and after firmware update finished, we should
 *  turn on the esd protector.
 */
static int goodix_esd_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct goodix_ts_esd *ts_esd = container_of(nb,
			struct goodix_ts_esd, esd_notifier);

	switch (action) {
	case NOTIFY_FWUPDATE_START:
	case NOTIFY_SUSPEND:
	case NOTIFY_ESD_OFF:
		goodix_ts_esd_off(ts_esd->ts_core);
		break;
	case NOTIFY_FWUPDATE_END:
	case NOTIFY_RESUME:
	case NOTIFY_ESD_ON:
		goodix_ts_esd_on(ts_esd->ts_core);
		break;
	}

	return 0;
}

/**
 * goodix_ts_esd_init - initialize esd protection
 */
int goodix_ts_esd_init(struct goodix_ts_core *core)
{
	struct goodix_ts_esd *ts_esd = &core->ts_esd;
	struct goodix_ts_device *dev = core->ts_dev;
	u8 data = GOODIX_ESD_TICK_WRITE_DATA;
	int r;

	INIT_DELAYED_WORK(&ts_esd->esd_work, goodix_ts_esd_work);
	mutex_init(&ts_esd->esd_mutex);
	ts_esd->ts_core = core;
	ts_esd->esd_on = false;
	ts_esd->esd_notifier.notifier_call = goodix_esd_notifier_callback;
	station_goodix_ts_register_notifier(&ts_esd->esd_notifier);

	if (dev->hw_ops->check_hw && dev->reg.esd != 0) {
		/*init static esd*/
		if (dev->ic_type == IC_TYPE_NANJING) {
			r = dev->hw_ops->write_trans(core->ts_dev,
				0x8043, &data, 1);
			if (r < 0)
				ts_err("static ESD init ERROR, i2c write failed");
		}

		/*init dynamic esd*/
		r = dev->hw_ops->write_trans(core->ts_dev,
				core->ts_dev->reg.esd,
				&data, 1);
		if (r < 0)
			ts_err("dynamic ESD init ERROR, i2c write failed");

		goodix_ts_esd_on(core);
	}
	return 0;
}

/**
 * goodix_ts_suspend - Touchscreen suspend function
 * Called by PM/FB/EARLYSUSPEN module to put the device to  sleep
 */
static int goodix_ts_suspend(struct goodix_ts_core *core_data)
{
	struct goodix_ext_module *ext_module;
	struct goodix_ts_device *ts_dev = core_data->ts_dev;
	int r;

	ts_info("Suspend start");

	/*
	 * notify suspend event, inform the esd protector
	 * and charger detector to turn off the work
	 */
	station_goodix_ts_blocking_notify(NOTIFY_SUSPEND, NULL);

	/* inform external module */
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->before_suspend)
				continue;

			r = ext_module->funcs->before_suspend(core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

	/* disable irq */
	station_goodix_ts_irq_enable(core_data, false);

	/* let touch ic work in sleep mode */
	if (ts_dev && ts_dev->hw_ops->suspend)
		ts_dev->hw_ops->suspend(ts_dev);
	atomic_set(&core_data->suspended, 1);

/*
#ifdef CONFIG_PINCTRL
	if (core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
				core_data->pin_sta_suspend);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif*/

	/* inform exteranl modules */
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->after_suspend)
				continue;

			r = ext_module->funcs->after_suspend(core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

out:
	/* release all the touch IDs */
	core_data->ts_event.event_data.touch_data.touch_num = 0;
	goodix_ts_input_report(core_data->input_dev,
			&core_data->ts_event.event_data.touch_data);
	ts_info("Suspend end");
	return 0;
}

/**
 * goodix_ts_resume - Touchscreen resume function
 * Called by PM/FB/EARLYSUSPEN module to wakeup device
 */
static int goodix_ts_resume(struct goodix_ts_core *core_data)
{
	struct goodix_ext_module *ext_module;
	struct goodix_ts_device *ts_dev =
				core_data->ts_dev;
	int r;

	ts_info("Resume start");
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->before_resume)
				continue;

			r = ext_module->funcs->before_resume(core_data, ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

/*#ifdef CONFIG_PINCTRL
	if (core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
					core_data->pin_sta_active);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif*/

	atomic_set(&core_data->suspended, 0);
	/* resume device */
	if (ts_dev && ts_dev->hw_ops->resume)
		ts_dev->hw_ops->resume(ts_dev);

	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->after_resume)
				continue;

			r = ext_module->funcs->after_resume(core_data, ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

	station_goodix_ts_irq_enable(core_data, true);
out:
	/*
	 * notify resume event, inform the esd protector
	 * and charger detector to turn on the work
	 */
	station_goodix_ts_blocking_notify(NOTIFY_RESUME, NULL);
	ts_debug("Resume end");
	return 0;
}

/**
 * goodix_ts_suspend - Touchscreen suspend function
 * Called by PM/FB/EARLYSUSPEN module to put the device to  sleep
 */
static void goodix_station_suspend_work(struct work_struct *work)
{
	struct goodix_ext_module *ext_module;
	struct goodix_ts_device *ts_dev = gts_core_data->ts_dev;
	int r;

	ts_info("Suspend start");
	mutex_lock(&gts_core_data->gts_station_suspend_mutex);

	/*
	 * notify suspend event, inform the esd protector
	 * and charger detector to turn off the work
	 */
	station_goodix_ts_blocking_notify(NOTIFY_SUSPEND, NULL);

	/* inform external module */
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->before_suspend)
				continue;

			r = ext_module->funcs->before_suspend(gts_core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

	/* disable irq */
	station_goodix_ts_irq_enable(gts_core_data, false);

	/* let touch ic work in sleep mode */
	if (ts_dev && ts_dev->hw_ops->suspend)
		ts_dev->hw_ops->suspend(ts_dev);
	atomic_set(&gts_core_data->suspended, 1);

/*
#ifdef CONFIG_PINCTRL
	if (gts_core_data->pinctrl) {
		r = pinctrl_select_state(gts_core_data->pinctrl,
				gts_core_data->pin_sta_suspend);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif*/

	/* inform exteranl modules */
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->after_suspend)
				continue;

			r = ext_module->funcs->after_suspend(gts_core_data, ext_module);
			if (r == EVT_CANCEL_SUSPEND) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

out:
	/* release all the touch IDs */
	gts_core_data->ts_event.event_data.touch_data.touch_num = 0;
	goodix_ts_input_report(gts_core_data->input_dev,
			&gts_core_data->ts_event.event_data.touch_data);
	mutex_unlock(&gts_core_data->gts_station_suspend_mutex);
	ts_info("Suspend end");
	return;
}

/**
 * goodix_ts_resume - Touchscreen resume function
 * Called by PM/FB/EARLYSUSPEN module to wakeup device
 */
static void goodix_station_resume_work(struct work_struct *work)
{
	struct goodix_ext_module *ext_module;
	struct goodix_ts_device *ts_dev =
				gts_core_data->ts_dev;
	int r;

	ts_info("Resume start");
	mutex_lock(&gts_core_data->gts_station_suspend_mutex);
	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->before_resume)
				continue;

			r = ext_module->funcs->before_resume(gts_core_data, ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

/*#ifdef CONFIG_PINCTRL
	if (gts_core_data->pinctrl) {
		r = pinctrl_select_state(gts_core_data->pinctrl,
					gts_core_data->pin_sta_active);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif*/

	atomic_set(&gts_core_data->suspended, 0);
	/* resume device */
	if (ts_dev && ts_dev->hw_ops->resume)
		ts_dev->hw_ops->resume(ts_dev);

	mutex_lock(&goodix_modules.mutex);
	if (!list_empty(&goodix_modules.head)) {
		list_for_each_entry(ext_module, &goodix_modules.head, list) {
			if (!ext_module->funcs->after_resume)
				continue;

			r = ext_module->funcs->after_resume(gts_core_data, ext_module);
			if (r == EVT_CANCEL_RESUME) {
				mutex_unlock(&goodix_modules.mutex);
				ts_info("Canceled by module:%s", ext_module->name);
				goto out;
			}
		}
	}
	mutex_unlock(&goodix_modules.mutex);

	station_goodix_ts_irq_enable(gts_core_data, true);
out:
	/*
	 * notify resume event, inform the esd protector
	 * and charger detector to turn on the work
	 */
	station_goodix_ts_blocking_notify(NOTIFY_RESUME, NULL);
	mutex_unlock(&gts_core_data->gts_station_suspend_mutex);
	ts_debug("Resume end");
	return;
}

#ifdef CONFIG_FB
/**
 * goodix_ts_fb_notifier_callback - Framebuffer notifier callback
 * Called by kernel during framebuffer blanck/unblank phrase
 */
int goodix_ts_fb_notifier_callback(struct notifier_block *self,
	unsigned long event, void *data)
{
	struct goodix_ts_core *core_data =
		container_of(self, struct goodix_ts_core, fb_notifier);
	struct msm_drm_notifier *evdata = data;
	int *blank;
	int id;

	if (evdata && evdata->data && core_data) {
		if (event == MSM_DRM_EARLY_EVENT_BLANK) {
			/* before fb blank */
			blank = evdata->data;
			id = evdata->id;
			if (*blank == MSM_DRM_BLANK_UNBLANK){
				ts_info("receives EARLY_BLANK:UNBLANK");
			}
			else if (*blank == MSM_DRM_BLANK_POWERDOWN){
				if (id == 0 && goodix_station_ts_success == 1) {
					//fts_suspend_running = true;
					queue_work(core_data->gts_station_suspend_resume_wq, &core_data->gts_station_suspend_work);
					ts_info("receives EARLY_BLANK:POWERDOWN from crtx(%d), run suspend", id);
				} else {
					ts_info("receives EARLY_BLANK:POWERDOWN from crtx(%d), don't run suspend", id);
				}
			}
		} else if (event == MSM_DRM_EVENT_BLANK) {
			blank = evdata->data;
			id = evdata->id;
			if (*blank == MSM_DRM_BLANK_UNBLANK){
				if (id == 0 && goodix_station_ts_success == 1) {
					//fts_suspend_running = false;
					queue_work(core_data->gts_station_suspend_resume_wq, &core_data->gts_station_resume_work);
					ts_info("receives BLANK:UNBLANK, run resume");
				} else {
					ts_info("focal touch not work skip resume");
				}
			}
			else if (*blank == MSM_DRM_BLANK_POWERDOWN){
				ts_info("receives BLANK:POWERDOWN");
			}
		}
	}
	return 0;
}
#endif

#ifdef CONFIG_HAS_EARLYSUSPEND
/**
 * goodix_ts_earlysuspend - Early suspend function
 * Called by kernel during system suspend phrase
 */
static void goodix_ts_earlysuspend(struct early_suspend *h)
{
	struct goodix_ts_core *core_data =
		container_of(h, struct goodix_ts_core,
			 early_suspend);

	goodix_ts_suspend(core_data);
}
/**
 * goodix_ts_lateresume - Late resume function
 * Called by kernel during system wakeup
 */
static void goodix_ts_lateresume(struct early_suspend *h)
{
	struct goodix_ts_core *core_data =
		container_of(h, struct goodix_ts_core,
			 early_suspend);

	goodix_ts_resume(core_data);
}
#endif

/**
 * goodix_ts_pm_suspend - PM suspend function
 * Called by kernel during system suspend phrase
 */
static int goodix_ts_pm_suspend(struct device *dev)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
        ts_info("goodix_ts_pm_suspend");
	return goodix_ts_suspend(core_data);
}
/**
 * goodix_ts_pm_resume - PM resume function
 * Called by kernel during system wakeup
 */
static int goodix_ts_pm_resume(struct device *dev)
{
	struct goodix_ts_core *core_data =
		dev_get_drvdata(dev);
        ts_info("goodix_ts_pm_resume");
	return goodix_ts_resume(core_data);
}

/**
 * goodix_generic_noti_callback - generic notifier callback
 *  for goodix touch notification event.
 */
int goodix_generic_noti_callback(struct notifier_block *self,
		unsigned long action, void *data)
{
	struct goodix_ts_core *ts_core = container_of(self,
			struct goodix_ts_core, ts_notifier);
	const struct goodix_ts_hw_ops *hw_ops = ts_hw_ops(ts_core);
	int r;

	switch (action) {
	case NOTIFY_FWUPDATE_END:
		if (hw_ops->init) {
			/* Firmware has been updated, we need to reinit
			 * the chip, read the sensor ID and send the
			 * correct config data based on sensor ID.
			 * The input parameters also needs to be updated.*/
			r = hw_ops->init(ts_core->ts_dev);
			if (r < 0)
				goto exit;

			goodix_ts_set_input_params(ts_core->input_dev,
					ts_core->ts_dev->board_data);
		}
		break;
	}

exit:
	return 0;

}


/**
 * goodix_ts_probe - called by kernel when a Goodix touch
 *  platform driver is added.
 */
static int goodix_ts_probe(struct platform_device *pdev)
{
	struct goodix_ts_core *core_data = NULL;
	struct goodix_ts_device *ts_device;
	int r;
	u8 read_val = 0;

	ts_info("goodix_ts_probe IN");

	ts_device = pdev->dev.platform_data;
	if (!ts_device || !ts_device->hw_ops ||
			!ts_device->board_data) {
		ts_err("Invalid touch device");
		return -ENODEV;
	}

	core_data = devm_kzalloc(&pdev->dev, sizeof(struct goodix_ts_core),
						GFP_KERNEL);
	if (!core_data) {
		ts_err("Failed to allocate memory for core data");
		return -ENOMEM;
	}

	/* touch core layer is a platform driver */
	core_data->pdev = pdev;
	core_data->ts_dev = ts_device;
	platform_set_drvdata(pdev, core_data);
	core_data->cfg_group_parsed = false;

	r = goodix_ts_power_init(core_data);
	if (r < 0)
		goto out;

	r = goodix_ts_power_on(core_data);
	if (r < 0)
		goto out;

/*#ifdef CONFIG_PINCTRL
	//Pinctrl handle is optional.
	r = goodix_ts_pinctrl_init(core_data);
	if (!r && core_data->pinctrl) {
		r = pinctrl_select_state(core_data->pinctrl,
					core_data->pin_sta_active);
		if (r < 0)
			ts_err("Failed to select active pinstate, r:%d", r);
	}
#endif*/

	/* get GPIO resource */
	r = goodix_ts_gpio_setup(core_data);
	if (r < 0)
		goto out;

	r = ts_device->hw_ops->reset(ts_device);
	if (r < 0)
		goto out;

	/*i2c test*/
	r = ts_device->hw_ops->read_trans(ts_device, 0x3100,
			&read_val, 1);
	if (!r)
		ts_info("i2c test SUCCESS, read_val is %x", read_val);
	else {
		ts_err("i2c test FAILED");
		goto out;
	}

	/*create sysfs files*/
	r = goodix_ts_sysfs_init(core_data);
	if (r < 0)
		goto out;

	/*unified protocl
	 * start a thread to parse cfg_bin and init IC*/
	r = goodix_start_cfg_bin(core_data);
	if (!r) {
		ts_info("***start cfg_bin_proc SUCCESS");
	} else {
		ts_err("***start cfg_bin_proc FAILED");
		goto out;
	}

	mutex_init(&core_data->gts_station_suspend_mutex);

	core_data->gts_station_suspend_resume_wq = create_singlethread_workqueue("goodix_station_suspend_resume_wq");
	if (NULL == core_data->gts_station_suspend_resume_wq) {
		ts_err("create suspend/resume workqueue failed");
		r=-1;
	}

	INIT_WORK(&core_data->gts_station_resume_work, goodix_station_resume_work);
	INIT_WORK(&core_data->gts_station_suspend_work, goodix_station_suspend_work);
	gts_core_data = core_data;

	goodix_station_ts_success = 1;

out:
	ts_info("goodix_ts_probe OUT, r:%d", r);
	return r;
}

static int goodix_ts_remove(struct platform_device *pdev)
{
	struct goodix_ts_core *core_data =
		platform_get_drvdata(pdev);

	ts_info("goodix_ts_remove start");
	goodix_station_ts_success = 0;

	//goodix 20190417
#ifdef CONFIG_FB
	//fb_unregister_client(&core_data->fb_notifier);
	msm_drm_unregister_client(&core_data->fb_notifier);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&core_data->early_suspend);
#endif	
	//
	cancel_work_sync(&core_data->gts_station_suspend_work);
	cancel_work_sync(&core_data->gts_station_resume_work);

	if (core_data->gts_station_suspend_resume_wq) {
		ts_info("goodix_ts_remove destroy_workqueue");
		destroy_workqueue(core_data->gts_station_suspend_resume_wq);
	}

	goodix_ts_esd_off(core_data);
	station_goodix_ts_irq_enable(core_data, false);
	goodix_ts_power_off(core_data);
	goodix_debugfs_exit();
	goodix_ts_sysfs_exit(core_data);

	ts_info("goodix_ts_remove exit");
	return 0;
}

static const struct dev_pm_ops dev_pm_ops = {
	.suspend = goodix_ts_pm_suspend,
	.resume = goodix_ts_pm_resume,
};

static const struct platform_device_id ts_core_ids[] = {
	{.name = GOODIX_CORE_DRIVER_NAME},
	{}
};
MODULE_DEVICE_TABLE(platform, ts_core_ids);

static struct platform_driver goodix_ts_driver = {
	.driver = {
		.name = GOODIX_CORE_DRIVER_NAME,
		.owner = THIS_MODULE,
		//.pm = &dev_pm_ops,
	},
	.probe = goodix_ts_probe,
	.remove = goodix_ts_remove,
	.id_table = ts_core_ids,
};

static int __init goodix_ts_core_init(void)
{
	ts_info("Core layer init");

	if (!goodix_modules.initilized) {
	        ts_info("initilize goodix module");
		goodix_modules.initilized = true;
		goodix_modules.core_exit = true;
		INIT_LIST_HEAD(&goodix_modules.head);
		mutex_init(&goodix_modules.mutex);
		init_completion(&goodix_modules.core_comp);
	}
	ts_info("goodix_debugfs_init");
	goodix_debugfs_init();
	return platform_driver_register(&goodix_ts_driver);
}


static void __exit goodix_ts_core_exit(void)
{
	ts_info("Core layer exit");
	platform_driver_unregister(&goodix_ts_driver);
	return;
}

static int __init goodix_ts_module_init(void)
{
	goodix_ts_core_init();
	goodix_i2c_init();
	return 0;
}

static void __exit goodix_ts_module_exit(void)
{
	if ( goodix_ts_fw_update == 1 )
		goodix_fwu_module_exit();
	//goodix_tools_exit();
	goodix_i2c_exit();
	goodix_ts_core_exit();
}

module_init(goodix_ts_module_init);
module_exit(goodix_ts_module_exit);

MODULE_DESCRIPTION("Goodix Touchscreen Core Module");
MODULE_AUTHOR("Goodix, Inc.");
MODULE_LICENSE("GPL v2");
