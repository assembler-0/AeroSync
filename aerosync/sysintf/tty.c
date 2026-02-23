/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/tty.c
 * @brief TTY/Serial class implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/sysintf/tty.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/export.h>
#include <mm/slub.h>

#include <lib/ringbuf.h>

static struct class tty_class = {
    .name = "tty",
    .dev_prefix = STRINGIFY(CONFIG_SERIAL_NAME_PREFIX),
    .naming_scheme = NAMING_NUMERIC,
    .category = DEV_CAT_TTY,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct device_driver tty_driver = {
    .name = "tty_core",
};

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

  /* Block until data is available or non-blocking? For now, non-blocking */
  return ringbuf_read(tty->read_buf, buf, count);
}

static ssize_t __no_cfi tty_cdev_write(struct char_device *cdev, const void *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct tty_struct *tty = cdev->private_data;
  if (!tty || !tty->ops || !tty->ops->write) return -EIO;

  return tty->ops->write(tty, buf, count);
}

static struct char_operations tty_char_ops = {
    .open = (void*)tty_cdev_open,
    .read = (void*)tty_cdev_read,
    .write = (void*)tty_cdev_write,
};

const struct char_operations *tty_get_char_ops(void) {
    return &tty_char_ops;
}
EXPORT_SYMBOL(tty_get_char_ops);

struct char_device *tty_register_device(const struct char_operations *ops, void *private_data) {
    (void)ops; // We use our internal tty_char_ops to wrap the tty_struct
    
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

    mutex_init(&tty->lock);
    tty->read_buf = ringbuf_create(4096);
    tty->ops = (const struct tty_operations *)ops;
    tty->driver_data = private_data;
    tty->cdev = cdev;

    cdev->dev.class = &tty_class;
    cdev->dev.driver = &tty_driver;
    cdev->ops = &tty_char_ops;
    cdev->private_data = tty;
    
    int id = ida_alloc(&tty_class.ida, GFP_KERNEL);
  if (id < 0) {
    ringbuf_destroy(tty->read_buf);
    kfree(tty);
    kfree(cdev);
    return nullptr;
  }
  cdev->dev.id = id;
  cdev->dev_num = MKDEV(4, 64 + id);

  if (char_device_register(cdev) != 0) {
    ida_free(&tty_class.ida, id);
    ringbuf_destroy(tty->read_buf);
    kfree(tty);
    kfree(cdev);
    return nullptr;
  }

  return cdev;
}

EXPORT_SYMBOL(tty_register_device);

void tty_receive_char(struct tty_struct *tty, char c) {
  if (tty && tty->read_buf) {
    ringbuf_write(tty->read_buf, &c, 1);
    /* TODO: Wake up waiters on read queue */
  }
}

EXPORT_SYMBOL(tty_receive_char);
