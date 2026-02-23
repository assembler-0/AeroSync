/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/char.c
 * @brief Character Device Registry
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
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

static LIST_HEAD(char_devices);
static struct mutex char_mutex;

static void char_init_subsystem(void) {
  static int initialized = 0;
  if (!initialized) {
    mutex_init(&char_mutex);
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

  if (kref_read(&cdev->dev.kref) == 0)
    device_initialize(&cdev->dev);

  int ret = device_add(&cdev->dev);
  if (ret != 0) {
    return ret;
  }

  mutex_lock(&char_mutex);
  list_add_tail(&cdev->list, &char_devices);
  mutex_unlock(&char_mutex);

  printk(KERN_INFO CHAR_CLASS "Registered character device '%s' (major: %u, minor: %u)\n",
         cdev->dev.name, MAJOR(cdev->dev_num), MINOR(cdev->dev_num));
  return 0;
}

EXPORT_SYMBOL(char_device_register);

void char_device_unregister(struct char_device *cdev) {
  if (!cdev)
    return;

  mutex_lock(&char_mutex);
  list_del(&cdev->list);
  mutex_unlock(&char_mutex);

  device_unregister(&cdev->dev);
}

EXPORT_SYMBOL(char_device_unregister);

struct char_device *chrdev_lookup(dev_t dev) {
  struct char_device *cdev;

  mutex_lock(&char_mutex);
  list_for_each_entry(cdev, &char_devices, list) {
    if (cdev->dev_num == dev) {
      kref_get(&cdev->dev.kref);
      mutex_unlock(&char_mutex);
      return cdev;
    }
  }
  mutex_unlock(&char_mutex);

  return nullptr;
}

EXPORT_SYMBOL(chrdev_lookup);
