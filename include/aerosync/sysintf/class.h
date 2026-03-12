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
#include <lib/id_alloc.h>

/**
 * enum naming_scheme - How to name devices in a class
 * @NAMING_NUMERIC: prefix + index (e.g. fb0, ttyS0)
 * @NAMING_ALPHABETIC: prefix + letter (e.g. hda, sdb)
 * @NAMING_NONE: no automatic naming (driver must set name)
 */
enum naming_scheme {
    NAMING_NUMERIC,
    NAMING_ALPHABETIC,
    NAMING_NONE
};

#define CLASS_FLAG_AUTO_DEVTMPFS (1 << 0) /* Automatically create devtmpfs nodes */

enum device_category {
    DEV_CAT_NONE,
    DEV_CAT_CHAR,
    DEV_CAT_BLOCK,
    DEV_CAT_TTY,
    DEV_CAT_FB
};

#include <linux/rcupdate.h>

/**
 * struct class - device classification structure
 * @name: name of the class (e.g. "input", "sound", "graphics")
 * @is_singleton: if true, the core tracks the 'active' device
 * @active_ops: [RCU] The active interface operations for this class
 * @active_dev: The device currently providing the active interface
 * @service_lock: Mutex protecting service re-evaluation
 * @evaluate: function to compare two devices to find the 'best' one
 */
struct class {
  const char *name;
  const char *dev_prefix;
  enum naming_scheme naming_scheme;
  enum device_category category;
  uint32_t flags;

  bool is_singleton;
  void *active_ops; /* RCU protected */
  struct device *active_dev;
  mutex_t service_lock;

  int (*evaluate)(struct device *a, struct device *b);

  struct list_head devices;
  mutex_t lock;
  struct ida ida;
  
  const char *dev_name;

  int (*dev_probe)(struct device *dev);
  void (*dev_release)(struct device *dev);
  void (*shutdown)(struct class *cls);

  struct list_head node;
};

/**
 * class_get_active_interface - Get the RCU-protected active interface for a class
 */
void *class_get_active_interface(struct class *cls);

/**
 * class_register - register a new class
 */
int class_register(struct class *cls);

/**
 * class_unregister - unregister a class
 */
void class_unregister(struct class *cls);

typedef int (*class_iter_fn)(struct device *dev, void *data);

/**
 * class_for_each_dev - iterate over devices in a class
 * @cls: the class to iterate
 * @start: the device to start after (nullptr for beginning)
 * @data: data to pass to the callback
 * @fn: callback function. If it returns non-zero, iteration stops and returns
 * that value.
 */
int class_for_each_dev(struct class *cls, struct device *start, void *data,
                       class_iter_fn fn);
