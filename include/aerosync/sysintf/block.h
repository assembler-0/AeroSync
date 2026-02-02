/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/block.h
 * @brief Block Device System Interface
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/mutex.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/types.h>
#include <linux/list.h>

#define BLOCK_NAME_MAX 32

struct block_device;

/**
 * Block device operations
 */
struct block_operations {
  /**
   * Read sectors from device
   * @param dev The block device
   * @param buffer Destination buffer
   * @param start_sector Starting LBA
   * @param sector_count Number of sectors to read
   * @return 0 on success, negative error code otherwise
   */
  int (*read)(struct block_device *dev, void *buffer, uint64_t start_sector,
              uint32_t sector_count);

  /**
   * Write sectors to device
   * @param dev The block device
   * @param buffer Source buffer
   * @param start_sector Starting LBA
   * @param sector_count Number of sectors to write
   * @return 0 on success, negative error code otherwise
   */
  int (*write)(struct block_device *dev, const void *buffer,
               uint64_t start_sector, uint32_t sector_count);

  /**
   * Flush device caches
   * @param dev The block device
   * @return 0 on success
   */
  int (*flush)(struct block_device *dev);

  /**
   * Optional: Close/Release the device
   */
  void (*release)(struct block_device *dev);
};

/**
 * Representation of a block device in the system
 */
struct block_device {
  struct device dev;
  char name[BLOCK_NAME_MAX];
  uint32_t id;
  dev_t dev_num; /* Major/Minor */

  uint32_t block_size;   // Usually 512 or 4096
  uint64_t sector_count; // Total size in sectors

  const struct block_operations *ops;
  void *private_data; // Driver-specific state

  struct list_head node; // Entry in global block device list
  mutex_t lock;          // Device-level exclusion

  /* Partition Support */
  struct block_device *parent_disk; /* If this is a partition, points to the master disk */
  uint64_t partition_offset;        /* Offset in sectors from the start of the parent disk */
};

/* --- Registration API --- */

/**
 * Register a new block device with the system
 * @return 0 on success
 */
int block_device_register(struct block_device *dev);

/**
 * Unregister a block device
 */
void block_device_unregister(struct block_device *dev);

/**
 * Assign a standardized name to a block device
 * @param dev The block device
 * @param prefix The prefix (e.g., "sd", "hd", "nvme")
 * @param index The 0-based index of the device
 * @return 0 on success
 */
int block_device_assign_name(struct block_device *dev, const char *prefix, int index);

/**
 * Assign a standardized ATAPI name to a block device (e.g., "cdrom0")
 */
int block_device_assign_atapi_name(struct block_device *dev, int index);

/**
 * Scan for partitions on a block device and register them
 * @param dev The disk to scan
 * @return Number of partitions found, or negative error
 */
int block_partition_scan(struct block_device *dev);

/* --- High-level I/O API --- */

/**
 * Standardized read/write helpers that handle locking and validation
 */
int block_read(struct block_device *dev, void *buffer, uint64_t start_sector,
               uint32_t sector_count);
int block_write(struct block_device *dev, const void *buffer,
                uint64_t start_sector, uint32_t sector_count);
int block_flush(struct block_device *dev);

/**
 * Lookup a device by name (e.g., "nvme0n1")
 */
struct block_device *block_device_find(const char *name);

/**
 * Helper to get device size in bytes
 */
static inline uint64_t block_device_size_bytes(struct block_device *dev) {
  return dev->sector_count * dev->block_size;
}
