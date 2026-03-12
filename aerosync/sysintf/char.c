/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/char.c
 * @brief Character Device Registry (Unified Model)
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/rcu.h>
#include <linux/rculist.h>
#include <aerosync/classes.h>
#include <lib/printk.h>

static struct class char_class = {
    .name = "char",
    .dev_prefix = "char",
    .naming_scheme = NAMING_NUMERIC,
    .category = DEV_CAT_CHAR,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct device_driver char_driver = {
    .name = "char_core",
};

static void char_init_subsystem(void) {
  static int initialized = 0;
  if (!initialized) {
    class_register(&char_class);
    initialized = 1;
  }
}

int char_device_register(struct char_device *cdev) {
    char_init_subsystem();
    
    if (!cdev || !cdev->ops)
        return -EINVAL;
        
    if (!cdev->dev.class)
        cdev->dev.class = &char_class;
    
    if (!cdev->dev.driver)
        cdev->dev.driver = &char_driver;

  return device_register(&cdev->dev);
}
EXPORT_SYMBOL(char_device_register);

void char_device_unregister(struct char_device *cdev) {
  if (!cdev) return;
  device_unregister(&cdev->dev);
}
EXPORT_SYMBOL(char_device_unregister);

struct char_device *chrdev_lookup(dev_t dev) {
  struct device *d;
  struct char_device *found = nullptr;

  rcu_read_lock();
  /* Use the generic class walk provided by the driver model */
  list_for_each_entry_rcu(d, &char_class.devices, class_node) {
    struct char_device *cdev = container_of(d, struct char_device, dev);
    if (cdev->dev_num == dev) {
      if (get_device(d)) {
        found = cdev;
      }
      break;
    }
  }
  rcu_read_unlock();

  return found;
}
EXPORT_SYMBOL(chrdev_lookup);
