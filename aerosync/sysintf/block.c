///SPDX-License-Identifier: GPL-2.0-only
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
#include <lib/string.h> /* For snprintf */
#include <aerosync/resdomain.h>
#include <aerosync/sched/process.h>

static struct class block_class = {
    .name = "block",
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct class ide_class = {
    .name = "ide",
    .dev_prefix = STRINGIFY(CONFIG_IDE_NAME_PREFIX),
    .naming_scheme = NAMING_ALPHABETIC,
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct class sata_class = {
    .name = "sata",
    .dev_prefix = STRINGIFY(CONFIG_SATA_NAME_PREFIX),
    .naming_scheme = NAMING_ALPHABETIC,
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct class nvme_class = {
    .name = "nvme",
    .dev_prefix = STRINGIFY(CONFIG_NVME_NAME_PREFIX),
    .naming_scheme = NAMING_NUMERIC,
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct class cdrom_class = {
    .name = "cdrom",
    .dev_prefix = STRINGIFY(CONFIG_CDROM_NAME_PREFIX),
    .naming_scheme = NAMING_NUMERIC,
    .category = DEV_CAT_BLOCK,
    .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct device_driver block_driver = {
    .name = "block_core",
};

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

  if (!dev || !dev->ops || !dev->ops->read)
    return -EINVAL;

  mutex_init(&dev->lock);
  
  if (!dev->dev.class)
      dev->dev.class = &block_class;
  
  if (!dev->dev.driver)
      dev->dev.driver = &block_driver;
  
  /* Initialize the device structure if not already done */
  if (kref_read(&dev->dev.kref) == 0)
    device_initialize(&dev->dev);
  
  /* Set parent-child relationship if this is a partition */
  if (dev->parent_disk) {
      dev->dev.parent = &dev->parent_disk->dev;
  }

  int ret = device_add(&dev->dev);
  if (ret != 0) {
    return ret;
  }

  /* Sync legacy name for compatibility */
  if (dev->dev.name) {
    strncpy(dev->name, dev->dev.name, BLOCK_NAME_MAX);
  }

  printk(KERN_INFO BLOCK_CLASS
         "Registered block device '%s' (%llu sectors, %u bytes/sector)\n",
         dev->dev.name, dev->sector_count, dev->block_size);

  return 0;
}
EXPORT_SYMBOL(block_device_register);

/* --- Naming Support --- */

int block_device_assign_name(struct block_device *dev, const char *prefix, int index) {
    /* 
     * Strategy: Match prefix to subclass.
     */
    if (strcmp(prefix, STRINGIFY(CONFIG_IDE_NAME_PREFIX)) == 0) {
        dev->dev.class = &ide_class;
    } else if (strcmp(prefix, STRINGIFY(CONFIG_SATA_NAME_PREFIX)) == 0) {
        dev->dev.class = &sata_class;
    } else if (strcmp(prefix, STRINGIFY(CONFIG_NVME_NAME_PREFIX)) == 0) {
        dev->dev.class = &nvme_class;
    }
    
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

/* --- Partition Scanning --- */

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

static void partition_release(struct block_device *bdev) {
    /* Just free the structure, parent handling is done by device model */
    kfree(bdev);
}

/* Wrapper operations for partitions */
static int part_read(struct block_device *dev, void *buffer, uint64_t start_sector, uint32_t sector_count) {
    /* This should never be called directly if block_read logic is correct, 
     * but we provide it just in case someone calls ops directly. */
    if (dev->parent_disk) {
        return block_read(dev->parent_disk, buffer, start_sector + dev->partition_offset, sector_count);
    }
    return -EIO;
}

static int part_write(struct block_device *dev, const void *buffer, uint64_t start_sector, uint32_t sector_count) {
    if (dev->parent_disk) {
        return block_write(dev->parent_disk, buffer, start_sector + dev->partition_offset, sector_count);
    }
    return -EIO;
}

static struct block_operations part_ops = {
    .read = part_read,
    .write = part_write,
    .release = partition_release,
};

int block_partition_scan(struct block_device *dev) {
    if (!dev || dev->block_size != 512) return -EINVAL; /* Only 512B sectors supported for now */
    
    struct mbr *mbr = kmalloc(512);
    if (!mbr) return -ENOMEM;

    /* Read MBR (LBA 0) */
    int ret = block_read(dev, mbr, 0, 1);
    if (ret != 0) {
        kfree(mbr);
        return ret;
    }

    if (mbr->signature != 0xAA55) {
        kfree(mbr);
        return 0; /* Not an MBR disk */
    }

    int partitions_found = 0;
    for (int i = 0; i < 4; i++) {
        struct mbr_entry *e = &mbr->entries[i];
        
        if (e->type == 0) continue; /* Empty */
        
        struct block_device *part = kzalloc(sizeof(struct block_device));
        if (!part) continue;

        part->parent_disk = dev;
        part->partition_offset = e->lba_start;
        part->sector_count = e->sector_count;
        part->block_size = dev->block_size;
        part->ops = &part_ops;
        
        /* Name: parent name + number (1-based) */
        device_set_name(&part->dev, "%s%d", dev->dev.name, i + 1);
        
        if (block_device_register(part) == 0) {
            partitions_found++;
        } else {
            kfree(part);
        }
    }

    kfree(mbr);
    return partitions_found;
}
EXPORT_SYMBOL(block_partition_scan);

/* --- Lookup Logic --- */

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
  struct find_data fd = {.name = name, .found = nullptr};

  class_for_each_dev(&block_class, nullptr, &fd, block_match_name);

  return fd.found;
}
EXPORT_SYMBOL(block_device_find);

struct block_device *blkdev_lookup(dev_t dev) {
  block_init_subsystem();
  struct block_device *bdev;
  struct block_device *found = nullptr;

  /* Search in generic block class and its subclasses */
  mutex_lock(&block_class.lock);
  list_for_each_entry(bdev, &block_class.devices, dev.class_node) {
      if (bdev->dev_num == dev) {
          found = bdev;
          get_device(&bdev->dev);
          break;
      }
  }
  mutex_unlock(&block_class.lock);

  if (found) return found;

  /* Search subclasses if needed (IDE, SATA, NVME) */
  struct class *subclasses[] = {&ide_class, &sata_class, &nvme_class, &cdrom_class, nullptr};
  for (int i = 0; subclasses[i]; i++) {
      struct class *cls = subclasses[i];
      mutex_lock(&cls->lock);
      list_for_each_entry(bdev, &cls->devices, dev.class_node) {
          if (bdev->dev_num == dev) {
              found = bdev;
              get_device(&bdev->dev);
              break;
          }
      }
      mutex_unlock(&cls->lock);
      if (found) break;
  }

  return found;
}
EXPORT_SYMBOL(blkdev_lookup);

/* --- Dispatchers --- */

int __no_cfi block_read(struct block_device *dev, void *buffer, uint64_t start_sector,
               uint32_t sector_count) {
  if (!dev || !buffer)
    return -EINVAL;
    
  /* Handle partitions: redirect to parent with offset */
  if (dev->parent_disk) {
      /* Recursively call block_read on parent to handle nested partitions if needed,
       * or just add offset. Here we add offset and switch dev. */
      start_sector += dev->partition_offset;
      dev = dev->parent_disk;
  }
    
  if (start_sector + sector_count > dev->sector_count)
    return -ERANGE;

  if (current && current->rd) {
      resdomain_io_throttle(current->rd, sector_count * dev->block_size);
  }

  mutex_lock(&dev->lock);
  int ret = dev->ops->read(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_read);

int __no_cfi block_write(struct block_device *dev, const void *buffer,
                uint64_t start_sector, uint32_t sector_count) {
  if (!dev || !buffer)
    return -EINVAL;
    
  if (dev->parent_disk) {
      start_sector += dev->partition_offset;
      dev = dev->parent_disk;
  }
    
  if (!dev->ops->write)
    return -ENOSYS;
  if (start_sector + sector_count > dev->sector_count)
    return -ERANGE;

  if (current && current->rd) {
      resdomain_io_throttle(current->rd, sector_count * dev->block_size);
  }

  mutex_lock(&dev->lock);
  int ret = dev->ops->write(dev, buffer, start_sector, sector_count);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_write);

int __no_cfi block_flush(struct block_device *dev) {
  if (!dev)
    return -EINVAL;
    
  if (dev->parent_disk)
      dev = dev->parent_disk;

  if (!dev->ops->flush)
    return 0; // Success if not supported

  mutex_lock(&dev->lock);
  int ret = dev->ops->flush(dev);
  mutex_unlock(&dev->lock);

  return ret;
}
EXPORT_SYMBOL(block_flush);