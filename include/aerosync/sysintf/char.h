/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/char.h
 * @brief Character Device System Interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/mutex.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/types.h>
#include <fs/vfs.h>
#include <linux/list.h>

#define MAJOR(dev) ((unsigned int) ((dev) >> 20))
#define MINOR(dev) ((unsigned int) ((dev) & ((1U << 20) - 1)))
#define MKDEV(ma, mi) (((ma) << 20) | (mi))

struct char_device;

/**
 * Character device operations
 */
struct char_operations {
  int (*open)(struct char_device *dev);
  void (*close)(struct char_device *dev);
  ssize_t (*read)(struct char_device *dev, void *buf, size_t count, vfs_loff_t *ppos);
  ssize_t (*write)(struct char_device *dev, const void *buf, size_t count, vfs_loff_t *ppos);
  int (*ioctl)(struct char_device *dev, uint32_t cmd, void *arg);
};

/**
 * struct char_device - Character device structure
 */
struct char_device {
  struct device dev;
  const struct char_operations *ops;
  void *private_data;

  dev_t dev_num; /* Major/Minor number */
  struct list_head list; /* List for registry */
};

/**
 * char_device_register - Register a character device
 */
int char_device_register(struct char_device *cdev);

/**
 * char_device_unregister - Unregister a character device
 */
void char_device_unregister(struct char_device *cdev);

/**
 * chrdev_lookup - Find a character device by device number
 */
struct char_device *chrdev_lookup(dev_t dev);
