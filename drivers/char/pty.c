/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/char/pty.c
 * @brief Pseudo-Terminal (PTY) driver
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/tty.h>
#include <lib/ringbuf.h>
#include <lib/string.h>
#include <mm/slub.h>

struct pty_pair {
  struct tty_struct *master;
  struct tty_struct *slave;
  int index;
  struct list_head list;
};

static LIST_HEAD(pty_pairs);
static mutex_t pty_lock;

static struct class pty_slave_class = {
    .name = "pty_slave",
    .dev_prefix = CONFIG_PTY_SLAVE_PREFIX,
    .naming_scheme = NAMING_NUMERIC,
    .category = DEV_CAT_TTY,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static ssize_t pty_master_write(struct tty_struct *tty, const void *buf,
                                size_t count) {
  struct pty_pair *pair = tty->driver_data;
  /* Master write goes to slave read buffer */
  return ringbuf_write(pair->slave->read_buf, buf, count);
}

static ssize_t pty_slave_write(struct tty_struct *tty, const void *buf,
                               size_t count) {
  struct pty_pair *pair = tty->driver_data;
  /* Slave write goes to master read buffer */
  return ringbuf_write(pair->master->read_buf, buf, count);
}

static struct tty_operations pty_master_ops = {
    .priority = TTY_PRIORITY_LOW,
    .write = pty_master_write,
};

static struct tty_operations pty_slave_ops = {
    .priority = TTY_PRIORITY_LOW,
    .write = pty_slave_write,
};

/* Implementation of /runtime/devices/ptmx */
static int ptmx_open(struct char_device *cdev) {
  (void)cdev;

  struct pty_pair *pair = kzalloc(sizeof(struct pty_pair));
  if (!pair)
    return -ENOMEM;

  /* Master structure (not a device itself, but an FD backer) */
  pair->master = kzalloc(sizeof(struct tty_struct));
  pair->master->read_buf = ringbuf_create(4096);
  pair->master->ops = &pty_master_ops;
  pair->master->driver_data = pair;

  /* Slave structure (registered as a device) */
  pair->slave = kzalloc(sizeof(struct tty_struct));
  pair->slave->read_buf = ringbuf_create(4096);
  pair->slave->ops = &pty_slave_ops;
  pair->slave->driver_data = pair;

  /* Use tty_register_device-like logic but for our specific pty_slave_class */
  struct char_device *slave_cdev = kzalloc(sizeof(struct char_device));
  slave_cdev->dev.class = &pty_slave_class;
  slave_cdev->ops = tty_get_char_ops();
  slave_cdev->private_data = pair->slave;
  pair->slave->cdev = slave_cdev;

  /* Let device_add handle the naming via pty_slave_class policy */
  if (char_device_register(slave_cdev) != 0) {
    ringbuf_destroy(pair->master->read_buf);
    ringbuf_destroy(pair->slave->read_buf);
    kfree(pair->master);
    kfree(pair->slave);
    kfree(pair);
    kfree(slave_cdev);
    return -EIO;
  }

  mutex_lock(&pty_lock);
  list_add_tail(&pair->list, &pty_pairs);
  mutex_unlock(&pty_lock);

  return 0;
}

static struct char_operations ptmx_fops = {
    .open = ptmx_open,
};

static struct char_device ptmx_cdev;

int pty_init(void) {
  mutex_init(&pty_lock);
  class_register(&pty_slave_class);

  memset(&ptmx_cdev, 0, sizeof(struct char_device));
  ptmx_cdev.dev.name = "ptmx";
  ptmx_cdev.ops = &ptmx_fops;
  ptmx_cdev.dev_num = MKDEV(5, 2);

  char_device_register(&ptmx_cdev);
  return 0;
}