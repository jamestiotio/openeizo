// SPDX-License-Identifier: GPL-2.0

/*
 *  HID driver for Eizo EV FlexScan Monitors
 *
 *  Copyright (c) 2021 Mark Bolhuis <mark@bolhuis.dev>
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hid.h>

#include "eizo.h"


int eizo_set_value(struct hid_device *hdev, u32 usage, u8 *value, size_t len) {
    struct eizo_data *data;
    u16 counter;
    u8 *report;
    int ret;

    data = hid_get_drvdata(hdev);
    if (IS_ERR(data)) {
        return -ENODATA;
    }

    report = kzalloc(39, GFP_KERNEL);
    if (IS_ERR(report)) {
        return -ENOMEM;
    }

    mutex_lock(&data->lock);
    counter = data->counter;

    report[1] = (usage >> 0) & 0xff;
    report[2] = (usage >> 8) & 0xff;
    report[3] = (usage >> 16) & 0xff;
    report[4] = (usage >> 24) & 0xff;

    report[5] = (counter >> 0) & 0xff;
    report[6] = (counter >> 8) & 0xff;

    memcpy(&report[7], value, len);

    ret = hid_hw_raw_request(hdev, 2, report, 39, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
    if (ret < 0) {
        hid_err(hdev, "failed to set hid report: %d\n", ret);
        goto exit;
    }

    ret = 0;
exit:
    mutex_unlock(&data->lock);
    kfree(report);
    return ret;
}

int eizo_get_value(struct hid_device *hdev, u32 usage, u8 *value, size_t len) {
    struct eizo_data *data;
    u16 counter;
    u8 *report;
    int ret;

    data = hid_get_drvdata(hdev);
    if (IS_ERR(data)) {
        hid_err(hdev, "failed to get eizo_data\n");
        return -ENODATA;
    }

    report = kzalloc(39, GFP_KERNEL);
    if (IS_ERR(report)) {
        hid_err(hdev, "failed to allocate report buffer");
        return -ENOMEM;
    }

    mutex_lock(&data->lock);
    counter = data->counter;

    report[1] = (usage >>  0) & 0xff;
    report[2] = (usage >>  8) & 0xff;
    report[3] = (usage >> 16) & 0xff;
    report[4] = (usage >> 24) & 0xff;

    report[5] = (counter >> 0) & 0xff;
    report[6] = (counter >> 8) & 0xff;

    ret = hid_hw_raw_request(hdev, 3, report, 39, HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
    if (ret < 0) {
        hid_err(hdev, "failed to set hid report: %d\n", ret);
        goto exit;
    }

    ret = hid_hw_raw_request(hdev, 3, report, 39, HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
    if (ret < 0) {
        hid_err(hdev, "failed to get hid report: %d\n", ret);
        goto exit;
    }

    memcpy(value, &report[7], len);

    ret = 0;
exit:
    mutex_unlock(&data->lock);
    kfree(report);
    return ret;
}


static ssize_t eizo_attr_store_brightness(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
    struct hid_device *hdev;
    u16 value;
    int res;

    res = kstrtou16(buf, 10, &value);
    if (res < 0) {
        return -EINVAL;
    }
    if (value < 0 || value > 200) {
        return -EOVERFLOW;
    }

    hdev = to_hid_device(dev);
    cpu_to_le16s(&value);
    res = eizo_set_value(hdev, EIZO_USAGE_BRIGHTNESS, (u8 *)&value, 2);
    if (res < 0) {
        hid_err(hdev, "failed to set %s value to %d, error %d\n", attr->attr.name, value, res);
        return res;
    }

    return count;
}

static ssize_t eizo_attr_show_brightness(struct device *dev, struct device_attribute *attr, char *buf) {
    struct hid_device *hdev;
    u16 value;
    int res;

    hdev = to_hid_device(dev);
    res = eizo_get_value(hdev, EIZO_USAGE_BRIGHTNESS, (u8 *)&value, 2);
    if (res < 0) {
        hid_err(hdev, "failed to get %s value, error %d\n", attr->attr.name, res);
        return -ENODATA;
    }

    le16_to_cpus(&value);
    return sprintf(buf, "%u\n", value);
}

static DEVICE_ATTR(brightness, 0664, eizo_attr_show_brightness, eizo_attr_store_brightness);


static struct attribute *eizo_attrs[] = {
        &dev_attr_brightness.attr,
        NULL
};

static struct attribute_group eizo_attr_group = {
        .name = "settings",
        .attrs = eizo_attrs,
};


void eizo_data_init(struct eizo_data *data, struct hid_device *hdev) {
    mutex_init(&data->lock);
    data->counter = 0x0001;
}

void eizo_data_uninit(struct eizo_data *data) {
    mutex_destroy(&data->lock);
}


static int eizo_hid_driver_probe(struct hid_device *hdev, const struct hid_device_id *id) {
    struct eizo_data *data;
    int retval = 0;

    data = devm_kzalloc(&hdev->dev, sizeof(struct eizo_data), GFP_KERNEL);
    if (IS_ERR(data)) {
        hid_err(hdev, "failed to allocate eizo_data\n");
        return -ENOMEM;
    }

    retval = hid_parse(hdev);
    if(retval < 0) {
        hid_err(hdev, "hid_parse failed\n");
        goto exit;
    }

    retval = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
    if (retval < 0) {
        hid_err(hdev, "hid_hw_start failed\n");
        goto exit;
    }

    hid_set_drvdata(hdev, data);
    eizo_data_init(data, hdev);

    retval = hid_hw_open(hdev);
    if (retval < 0) {
        hid_err(hdev, "hid_hw_open failed\n");
        goto stop;
    }

    retval = sysfs_create_group(&hdev->dev.kobj, &eizo_attr_group);
    if (retval < 0) {
        hid_err(hdev, "sysfs_create_group failed\n");
        goto close;
    }

    return 0;
close:
    hid_hw_close(hdev);
stop:
    hid_hw_stop(hdev);
exit:
    return retval;
}

static void eizo_hid_driver_remove(struct hid_device *hdev) {
    struct eizo_data *data;
    data = hid_get_drvdata(hdev);

    sysfs_remove_group(&hdev->dev.kobj, &eizo_attr_group);
    hid_hw_close(hdev);
    eizo_data_uninit(data);
    hid_hw_stop(hdev);
}

static int eizo_hid_driver_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size) {
    u32 usage, value;
    u16 counter;
    u8 id;

    switch(report->id) {
        case 0x02:
        case 0x03:
            id = data[0];
            usage = data[1] | (data[2] << 8) | (data[3] << 16) | (data[4] << 24);
            counter = data[5] | (data[6] << 8);
            value = data[7] | (data[8] << 8) | (data[9] << 16) | (data[10] << 24);

            hid_info(hdev, "event %d: %08x %04x %08x\n", id, usage, counter, value);
            break;

        default:
            hid_info(hdev, "event %d\n", report->id);
            break;
    }

    return 0;
}

static const struct hid_device_id eizo_hid_driver_id_table[] = {
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2450) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2451) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2455) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2456) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2457) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2460) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2750) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2760) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2785) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV2795) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV3237) },
        { HID_USB_DEVICE(USB_VENDOR_ID_EIZO, USB_PRODUCT_ID_EIZO_EV3285) },
        { }
};

MODULE_DEVICE_TABLE(hid, eizo_hid_driver_id_table);

static struct hid_driver eizo_hid_driver = {
        .name =         "eizo",
        .id_table =     eizo_hid_driver_id_table,
        .probe =        eizo_hid_driver_probe,
        .remove =       eizo_hid_driver_remove,
        .raw_event =    eizo_hid_driver_raw_event,
};

module_hid_driver(eizo_hid_driver);

MODULE_AUTHOR("Mark Bolhuis");
MODULE_DESCRIPTION("HID device driver for EIZO FlexScan monitors");
MODULE_LICENSE("GPL v2");