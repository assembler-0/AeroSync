/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/block.c
 * @brief Block Device Registry and Dispatcher
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sysintf/block.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/errno.h>
#include <linux/container_of.h>

static LIST_HEAD(block_devices);
static mutex_t block_list_lock;
static uint32_t next_device_id = 0;

static void block_init_subsystem(void) {
    static int initialized = 0;
    if (initialized) return;
    mutex_init(&block_list_lock);
    initialized = 1;
}

int block_device_register(struct block_device *dev) {
    block_init_subsystem();

    if (!dev || !dev->ops || !dev->ops->read)
        return -EINVAL;

    mutex_lock(&block_list_lock);
    
    dev->id = next_device_id++;
    mutex_init(&dev->lock);
    list_add_tail(&dev->node, &block_devices);
    
    mutex_unlock(&block_list_lock);

    printk(KERN_INFO BLOCK_CLASS "Registered device '%s' (%u sectors, %u bytes/sector)\n",
           dev->name, dev->sector_count, dev->block_size);

    return 0;
}
EXPORT_SYMBOL(block_device_register);

void block_device_unregister(struct block_device *dev) {
    if (!dev) return;

    mutex_lock(&block_list_lock);
    list_del(&dev->node);
    mutex_unlock(&block_list_lock);

    if (dev->ops->release)
        dev->ops->release(dev);

    printk(KERN_INFO BLOCK_CLASS "Unregistered device '%s'\n", dev->name);
}
EXPORT_SYMBOL(block_device_unregister);

struct block_device *block_device_find(const char *name) {
    block_init_subsystem();
    struct block_device *dev;

    mutex_lock(&block_list_lock);
    list_for_each_entry(dev, &block_devices, node) {
        if (strcmp(dev->name, name) == 0) {
            mutex_unlock(&block_list_lock);
            return dev;
        }
    }
    mutex_unlock(&block_list_lock);
    return NULL;
}
EXPORT_SYMBOL(block_device_find);

int block_read(struct block_device *dev, void *buffer, uint64_t start_sector, uint32_t sector_count) {
    if (!dev || !buffer) return -EINVAL;
    if (start_sector + sector_count > dev->sector_count) return -ERANGE;

    mutex_lock(&dev->lock);
    int ret = dev->ops->read(dev, buffer, start_sector, sector_count);
    mutex_unlock(&dev->lock);

    return ret;
}
EXPORT_SYMBOL(block_read);

int block_write(struct block_device *dev, const void *buffer, uint64_t start_sector, uint32_t sector_count) {
    if (!dev || !buffer) return -EINVAL;
    if (!dev->ops->write) return -ENOSYS;
    if (start_sector + sector_count > dev->sector_count) return -ERANGE;

    mutex_lock(&dev->lock);
    int ret = dev->ops->write(dev, buffer, start_sector, sector_count);
    mutex_unlock(&dev->lock);

    return ret;
}
EXPORT_SYMBOL(block_write);

int block_flush(struct block_device *dev) {
    if (!dev) return -EINVAL;
    if (!dev->ops->flush) return 0; // Success if not supported

    mutex_lock(&dev->lock);
    int ret = dev->ops->flush(dev);
    mutex_unlock(&dev->lock);

    return ret;
}
EXPORT_SYMBOL(block_flush);
