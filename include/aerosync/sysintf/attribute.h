/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/attribute.h
 * @brief Generic Attribute System (sysfs backing)
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>

struct device;
struct kobject;

/**
 * struct attribute - generic attribute
 * @name: Name of the attribute
 * @mode: Protection mode (0644, 0444, etc.)
 */
struct attribute {
    const char *name;
    uint16_t mode;
};

struct attribute_group {
    const char *name;
    struct attribute **attrs;
};

/**
 * struct device_attribute - attribute for a device
 * @attr: Underlying attribute
 * @show: Callback to read value
 * @store: Callback to write value
 */
struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device *dev, struct device_attribute *attr, char *buf);
    ssize_t (*store)(struct device *dev, struct device_attribute *attr, const char *buf, size_t count);
};

/* Macros to make defining attributes easier */
#define __ATTR(_name, _mode, _show, _store) { \
    .attr = { .name = #_name, .mode = _mode }, \
    .show = _show, \
    .store = _store, \
}

#define DEVICE_ATTR(_name, _mode, _show, _store) \
    struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)

#define DEVICE_ATTR_RO(_name) \
    struct device_attribute dev_attr_##_name = __ATTR(_name, 0444, _name##_show, nullptr)

#define DEVICE_ATTR_RW(_name) \
    struct device_attribute dev_attr_##_name = __ATTR(_name, 0644, _name##_show, _name##_store)
