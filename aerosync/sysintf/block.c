/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/block.c
 * @brief Block Device Registry and Dispatcher
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/block.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>

#include <aerosync/sysintf/class.h>
#include <mm/slub.h>

static struct class block_class = {
    .name = "block",
};

static atomic_t block_class_registered = ATOMIC_INIT(0);
static uint32_t next_device_id = 0;

static void block_init_subsystem(void) {
  if (atomic_xchg(&block_class_registered, 1) == 0) {
    class_register(&block_class);
  }
}

int block_device_register(struct block_device *dev) {
  block_init_subsystem();

  if (!dev || !dev->ops || !dev->ops->read)
    return -EINVAL;

  dev->id = next_device_id++;
  mutex_init(&dev->lock);

  dev->dev.class = &block_class;
  dev->dev.name = dev->name;

  int ret = device_register(&dev->dev);
  if (ret != 0) {
    return ret;
  }

  printk(KERN_INFO BLOCK_CLASS
         "Registered device '%s' (%u sectors, %u bytes/sector)\n",
         dev->name, dev->sector_count, dev->block_size);

  return 0;
}
EXPORT_SYMBOL(block_device_register);

void block_device_unregister(struct block_device *dev) {
  if (!dev)
    return;

  device_unregister(&dev->dev);

  if (dev->ops->release)
    dev->ops->release(dev);

  printk(KERN_INFO BLOCK_CLASS "Unregistered device '%s'\n", dev->name);
}
EXPORT_SYMBOL(block_device_unregister);

struct find_data {
  const char *name;
  struct block_device *found;
};

static int block_match_name(struct device *dev, void *data) {
  struct find_data *fd = data;
  struct block_device *bdev = container_of(dev, struct block_device, dev);

  if (strcmp(bdev->name, fd->name) == 0) {
    fd->found = bdev;
    return 1; // Stop iteration
  }
  return 0;
}

struct block_device *block_device_find(const char *name) {
  block_init_subsystem();
  struct find_data fd = {.name = name, .found = NULL};

  class_for_each_dev(&block_class, NULL, &fd, block_match_name);

  return fd.found;
}
EXPORT_SYMBOL(block_device_find);

int block_read(struct block_device *dev, void *buffer, uint64_t start_sector,
               uint32_t sector_count) {
  if (!dev || !buffer)
    return -EINVAL;
  if (start_sector + sector_count > dev->sector_count)
    return -ERANGE;

  mutex_lock(&dev->lock);
  int ret = dev->ops->read(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_read);

int block_write(struct block_device *dev, const void *buffer,
                uint64_t start_sector, uint32_t sector_count) {
  if (!dev || !buffer)
    return -EINVAL;
  if (!dev->ops->write)
    return -ENOSYS;
  if (start_sector + sector_count > dev->sector_count)
    return -ERANGE;

  mutex_lock(&dev->lock);
  int ret = dev->ops->write(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_write);

int block_flush(struct block_device *dev) {
  if (!dev)
    return -EINVAL;
  if (!dev->ops->flush)
    return 0; // Success if not supported

  mutex_lock(&dev->lock);
  int ret = dev->ops->flush(dev);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_flush);
