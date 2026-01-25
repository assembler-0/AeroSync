/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/class.h
 * @brief Unified Driver Model - Class structure
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/mutex.h>
#include <aerosync/types.h>
#include <linux/list.h>

struct device;

/**
 * struct class - device classification structure
 * @name: name of the class (e.g. "input", "sound", "graphics")
 * @class_attrs: default attributes for devices in this class
 * @devices: list of devices belonging to this class
 * @lock: lock for the devices list
 * @dev_probe: triggered when a device is added to this class
 * @dev_release: triggered when a device is removed from this class
 * @shutdown: optional callback at shutdown
 */
struct class {
  const char *name;

  struct list_head devices;
  mutex_t lock;

  int (*dev_probe)(struct device *dev);
  void (*dev_release)(struct device *dev);
  void (*shutdown)(struct class *cls);

  struct list_head node; /* internal node in global class list */
};

/**
 * class_register - register a new class
 */
int class_register(struct class *cls);

/**
 * class_unregister - unregister a class
 */
void class_unregister(struct class *cls);

/**
 * class_for_each_dev - iterate over devices in a class
 * @cls: the class to iterate
 * @start: the device to start after (NULL for beginning)
 * @data: data to pass to the callback
 * @fn: callback function. If it returns non-zero, iteration stops and returns
 * that value.
 */
int class_for_each_dev(struct class *cls, struct device *start, void *data,
                       int (*fn)(struct device *dev, void *data));
