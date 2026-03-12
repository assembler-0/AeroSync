/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/tty.c
 * @brief TTY/Serial class implementation (Unified Model)
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/sysintf/tty.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/export.h>
#include <mm/slub.h>
#include <lib/ringbuf.h>

static int tty_evaluate(struct device *a, struct device *b) {
  struct char_device *cdev_a = container_of(a, struct char_device, dev);
  struct char_device *cdev_b = container_of(b, struct char_device, dev);
  struct tty_struct *tty_a = (struct tty_struct *) cdev_a->private_data;
  struct tty_struct *tty_b = (struct tty_struct *) cdev_b->private_data;

  if (!tty_a || !tty_a->ops) return -1;
  if (!tty_b || !tty_b->ops) return 1;

  return (int) tty_a->ops->priority - (int) tty_b->ops->priority;
}

static int tty_device_probe(struct device *dev) {
  struct char_device *cdev = container_of(dev, struct char_device, dev);
  cdev->dev_num = MKDEV(4, 64 + dev->id);
  return 0;
}

static struct class tty_class = {
  .name = "tty",
  .dev_prefix = CONFIG_SERIAL_NAME_PREFIX,
  .naming_scheme = NAMING_NUMERIC,
  .category = DEV_CAT_TTY,
  .flags = CLASS_FLAG_AUTO_DEVTMPFS,
  .is_singleton = true, /* Tracks the primary console */
  .evaluate = tty_evaluate,
  .dev_probe = tty_device_probe,
};

static struct device_driver tty_driver = {
  .name = "tty_core",
};

static void tty_device_release(struct device *dev) {
  struct char_device *cdev = container_of(dev, struct char_device, dev);
  struct tty_struct *tty = cdev->private_data;

  if (tty) {
    if (tty->read_buf) {
      ringbuf_destroy(tty->read_buf);
    }
    kfree(tty);
  }
  kfree(cdev);
}

static int __no_cfi tty_cdev_open(struct char_device *cdev) {
  struct tty_struct *tty = cdev->private_data;
  if (tty && tty->ops && tty->ops->open) {
    return tty->ops->open(tty);
  }
  return 0;
}

static ssize_t tty_cdev_read(struct char_device *cdev, void *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct tty_struct *tty = cdev->private_data;
  if (!tty || !tty->read_buf) return -EIO;
  return (signed) ringbuf_read(tty->read_buf, buf, count);
}

static ssize_t __no_cfi tty_cdev_write(struct char_device *cdev, const void *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct tty_struct *tty = cdev->private_data;
  if (!tty || !tty->ops || !tty->ops->write) return -EIO;
  return tty->ops->write(tty, buf, count);
}

static struct char_operations tty_char_ops = {
  .open = (void *) tty_cdev_open,
  .read = (void *) tty_cdev_read,
  .write = (void *) tty_cdev_write,
};

const struct char_operations *tty_get_char_ops(void) {
  return &tty_char_ops;
}

EXPORT_SYMBOL(tty_get_char_ops);

struct char_device *tty_register_device(const struct tty_operations *ops, void *private_data) {
  static int initialized = 0;
  if (!initialized) {
    class_register(&tty_class);
    initialized = 1;
  }

  struct tty_struct *tty = kzalloc(sizeof(struct tty_struct));
  if (!tty) return nullptr;

  struct char_device *cdev = kzalloc(sizeof(struct char_device));
  if (!cdev) {
    kfree(tty);
    return nullptr;
  }

  device_initialize(&cdev->dev);
  cdev->dev.release = tty_device_release;

  mutex_init(&tty->lock);
  tty->read_buf = ringbuf_create(4096);
  tty->ops = ops;
  tty->driver_data = private_data;
  tty->cdev = cdev;

  cdev->dev.class = &tty_class;
  cdev->dev.driver = &tty_driver;
  cdev->dev.ops = (void *) &tty_char_ops; /* Expose unified char ops */
  cdev->ops = &tty_char_ops;
  cdev->private_data = tty;

  if (device_register(&cdev->dev) != 0) {
    put_device(&cdev->dev);
    return nullptr;
  }

  return cdev;
}

EXPORT_SYMBOL(tty_register_device);

void tty_unregister_device(struct char_device *cdev) {
  if (!cdev) return;
  device_unregister(&cdev->dev);
}

EXPORT_SYMBOL(tty_unregister_device);

void tty_receive_char(struct tty_struct *tty, char c) {
  if (tty && tty->read_buf) {
    ringbuf_write(tty->read_buf, &c, 1);
  }
}

EXPORT_SYMBOL(tty_receive_char);

struct tty_struct *tty_get_active(void) {
  void *ops = class_get_active_interface(&tty_class);
  if (!ops) return nullptr;

  /* The active device for TTY class has tty_struct in private_data */
  struct device *dev = tty_class.active_dev;
  if (!dev) return nullptr;

  struct char_device *cdev = container_of(dev, struct char_device, dev);
  return (struct tty_struct *) cdev->private_data;
}

EXPORT_SYMBOL(tty_get_active);
