///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/block.c
 * @brief Block Device Registry and Dispatcher (Unified Model)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/block.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/rcu.h>
#include <linux/rculist.h>
#include <aerosync/resdomain.h>
#include <aerosync/sched/process.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>

static int get_block_prio(const char *name) {
  if (name && strcmp(name, "nvme") == 0) return 3;
  if (name && strcmp(name, "sata") == 0) return 2;
  return 1;
}

static int block_evaluate(struct device *a, struct device *b) {
  /* Simple: NVME > SATA > IDE */
  return get_block_prio(a->class->name) - get_block_prio(b->class->name);
}

static struct class block_class = {
    .name = "block",
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
    .is_singleton = true, /* Primary block device */
    .evaluate = block_evaluate,
};

static struct class ide_class = { .name = "ide", .dev_prefix = CONFIG_IDE_NAME_PREFIX, .naming_scheme = NAMING_ALPHABETIC, .category = DEV_CAT_BLOCK, .flags = CLASS_FLAG_AUTO_DEVTMPFS };
static struct class sata_class = { .name = "sata", .dev_prefix = CONFIG_SATA_NAME_PREFIX, .naming_scheme = NAMING_ALPHABETIC, .category = DEV_CAT_BLOCK, .flags = CLASS_FLAG_AUTO_DEVTMPFS };
static struct class nvme_class = { .name = "nvme", .dev_prefix = CONFIG_NVME_NAME_PREFIX, .naming_scheme = NAMING_NUMERIC, .category = DEV_CAT_BLOCK, .flags = CLASS_FLAG_AUTO_DEVTMPFS };
static struct class cdrom_class = { .name = "cdrom", .dev_prefix = CONFIG_CDROM_NAME_PREFIX, .naming_scheme = NAMING_NUMERIC, .category = DEV_CAT_BLOCK, .flags = CLASS_FLAG_AUTO_DEVTMPFS };

static struct device_driver block_driver = { .name = "block_core" };

static void block_init_subsystem(void) {
  static int initialized = 0;
  if (!initialized) {
    class_register(&block_class);
    class_register(&ide_class);
    class_register(&sata_class);
    class_register(&nvme_class);
    class_register(&cdrom_class);
    initialized = 1;
  }
}

int block_device_register(struct block_device *dev) {
  block_init_subsystem();
  if (!dev || !dev->ops || !dev->ops->read) return -EINVAL;

  mutex_init(&dev->lock);
  if (!dev->dev.class) dev->dev.class = &block_class;
  if (!dev->dev.driver) dev->dev.driver = &block_driver;
  
  dev->dev.ops = (void *)dev->ops;

  if (kref_read(&dev->dev.kref) == 0) device_initialize(&dev->dev);
  if (dev->parent_disk) dev->dev.parent = &dev->parent_disk->dev;

  int ret = device_add(&dev->dev);
  if (ret == 0 && dev->dev.name) strncpy(dev->name, dev->dev.name, BLOCK_NAME_MAX);
  
  return ret;
}
EXPORT_SYMBOL(block_device_register);

void block_device_unregister(struct block_device *dev) {
  if (!dev) return;
  device_unregister(&dev->dev);
}
EXPORT_SYMBOL(block_device_unregister);

int block_device_assign_name(struct block_device *dev, const char *prefix, int index) {
  if (strcmp(prefix, CONFIG_IDE_NAME_PREFIX) == 0) dev->dev.class = &ide_class;
  else if (strcmp(prefix, CONFIG_SATA_NAME_PREFIX) == 0) dev->dev.class = &sata_class;
  else if (strcmp(prefix, CONFIG_NVME_NAME_PREFIX) == 0) dev->dev.class = &nvme_class;
  
  dev->dev.id = index;
  return 0;
}
EXPORT_SYMBOL(block_device_assign_name);

int block_device_assign_atapi_name(struct block_device *dev, int index) {
  dev->dev.class = &cdrom_class;
  dev->dev.id = index;
  return 0;
}
EXPORT_SYMBOL(block_device_assign_atapi_name);

/* --- MBR Partition Support --- */

struct mbr_entry {
  uint8_t status;
  uint8_t chs_first[3];
  uint8_t type;
  uint8_t chs_last[3];
  uint32_t lba_start;
  uint32_t sector_count;
} __packed;

struct mbr {
  uint8_t code[446];
  struct mbr_entry entries[4];
  uint16_t signature;
} __packed;

static void partition_release(struct device *dev) {
  struct block_device *bdev = container_of(dev, struct block_device, dev);
  kfree(bdev);
}

static int part_read(struct block_device *dev, void *buffer, uint64_t start_sector, uint32_t sector_count) {
  if (dev->parent_disk) return block_read(dev->parent_disk, buffer, start_sector + dev->partition_offset, sector_count);
  return -EIO;
}

static int part_write(struct block_device *dev, const void *buffer, uint64_t start_sector, uint32_t sector_count) {
  if (dev->parent_disk) return block_write(dev->parent_disk, buffer, start_sector + dev->partition_offset, sector_count);
  return -EIO;
}

static struct block_operations part_ops = {
  .read = part_read,
  .write = part_write,
};

int block_partition_scan(struct block_device *dev) {
  if (!dev || dev->block_size != 512) return -EINVAL;
  
  struct mbr *mbr = kmalloc(512);
  if (!mbr) return -ENOMEM;

  int ret = block_read(dev, mbr, 0, 1);
  if (ret != 0 || mbr->signature != 0xAA55) {
    kfree(mbr);
    return ret;
  }

  int partitions_found = 0;
  for (int i = 0; i < 4; i++) {
    struct mbr_entry *e = &mbr->entries[i];
    if (e->type == 0) continue;
    
    struct block_device *part = kzalloc(sizeof(struct block_device));
    if (!part) continue;

    part->parent_disk = dev;
    part->partition_offset = e->lba_start;
    part->sector_count = e->sector_count;
    part->block_size = dev->block_size;
    part->ops = &part_ops;
    part->dev.release = partition_release;
    
    device_set_name(&part->dev, "%s%d", dev->dev.name, i + 1);
    
    if (block_device_register(part) == 0) partitions_found++;
    else kfree(part);
  }

  kfree(mbr);
  return partitions_found;
}
EXPORT_SYMBOL(block_partition_scan);

/* --- Lookup Logic --- */

struct block_device *block_device_find(const char *name) {
  block_init_subsystem();
  struct device *dev = device_find_by_name(name);
  if (!dev) return nullptr;
  /* Verify it belongs to one of our block classes */
  if (dev->class->category != DEV_CAT_BLOCK) {
    put_device(dev);
    return nullptr;
  }
  return container_of(dev, struct block_device, dev);
}
EXPORT_SYMBOL(block_device_find);

struct block_device *blkdev_lookup(dev_t dev) {
  block_init_subsystem();
  struct device *d;
  struct block_device *found = nullptr;
  struct class *classes[] = {&block_class, &ide_class, &sata_class, &nvme_class, &cdrom_class, nullptr};

  rcu_read_lock();
  for (int i = 0; classes[i]; i++) {
    list_for_each_entry_rcu(d, &classes[i]->devices, class_node) {
      struct block_device *bdev = container_of(d, struct block_device, dev);
      if (bdev->dev_num == dev) {
        if (get_device(d)) found = bdev;
        break;
      }
    }
    if (found) break;
  }
  rcu_read_unlock();
  return found;
}
EXPORT_SYMBOL(blkdev_lookup);

struct block_device *block_get_primary(void) {
  void *ops = class_get_active_interface(&block_class);
  if (!ops) return nullptr;
  struct device *dev = block_class.active_dev;
  return container_of(dev, struct block_device, dev);
}
EXPORT_SYMBOL(block_get_primary);

/* Dispatchers */
int __no_cfi block_read(struct block_device *dev, void *buffer, uint64_t start_sector, uint32_t sector_count) {
  if (!dev || !buffer) return -EINVAL;
  if (dev->parent_disk) { start_sector += dev->partition_offset; dev = dev->parent_disk; }
  if (start_sector + sector_count > dev->sector_count) return -ERANGE;

  if (current && current->rd) resdomain_io_throttle(current->rd, sector_count * dev->block_size);

  mutex_lock(&dev->lock);
  int ret = dev->ops->read(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);
  return ret;
}
EXPORT_SYMBOL(block_read);

int __no_cfi block_write(struct block_device *dev, const void *buffer, uint64_t start_sector, uint32_t sector_count) {
  if (!dev || !buffer) return -EINVAL;
  if (dev->parent_disk) { start_sector += dev->partition_offset; dev = dev->parent_disk; }
  if (!dev->ops->write) return -ENOSYS;
  if (start_sector + sector_count > dev->sector_count) return -ERANGE;

  if (current && current->rd) resdomain_io_throttle(current->rd, sector_count * dev->block_size);

  mutex_lock(&dev->lock);
  int ret = dev->ops->write(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);
  return ret;
}
EXPORT_SYMBOL(block_write);

int __no_cfi block_flush(struct block_device *dev) {
  if (!dev) return -EINVAL;
  if (dev->parent_disk) dev = dev->parent_disk;
  if (!dev->ops->flush) return 0;

  mutex_lock(&dev->lock);
  int ret = dev->ops->flush(dev);
  mutex_unlock(&dev->lock);
  return ret;
}
EXPORT_SYMBOL(block_flush);
