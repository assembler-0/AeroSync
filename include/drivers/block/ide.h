/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/block/ide/ide.h
 * @brief Internal definitions for IDE/ATA driver
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/errno.h>
#include <arch/x86_64/io.h>
#include <aerosync/sysintf/block.h>
#include <aerosync/sysintf/pci.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/mutex.h>
#include <aerosync/completion.h>
#include <arch/x86_64/tsc.h>

/* ATA Register Offsets */
#define ATA_REG_DATA            0x00
#define ATA_REG_ERROR           0x01
#define ATA_REG_FEATURES        0x01
#define ATA_REG_SEC_COUNT       0x02
#define ATA_REG_LBA_LOW         0x03
#define ATA_REG_LBA_MID         0x04
#define ATA_REG_LBA_HIGH        0x05
#define ATA_REG_DRIVE           0x06
#define ATA_REG_COMMAND         0x07
#define ATA_REG_STATUS          0x07

/* Control Register */
#define ATA_REG_CONTROL         0x00 /* Offset from control base (0x3F6/0x376) */
#define ATA_REG_ALT_STATUS      0x00

/* Status Bits */
#define ATA_SR_BSY              0x80    /* Busy */
#define ATA_SR_DRDY             0x40    /* Drive ready */
#define ATA_SR_DF               0x20    /* Drive fault */
#define ATA_SR_DSC              0x10    /* Drive seek complete */
#define ATA_SR_DRQ              0x08    /* Data request ready */
#define ATA_SR_CORR             0x04    /* Corrected data */
#define ATA_SR_IDX              0x02    /* Index */
#define ATA_SR_ERR              0x01    /* Error */

/* Commands */
#define ATA_CMD_READ_PIO          0x20
#define ATA_CMD_READ_PIO_EXT      0x24
#define ATA_CMD_READ_DMA          0xC8
#define ATA_CMD_READ_DMA_EXT      0x25
#define ATA_CMD_WRITE_PIO         0x30
#define ATA_CMD_WRITE_PIO_EXT     0x34
#define ATA_CMD_WRITE_DMA         0xCA
#define ATA_CMD_WRITE_DMA_EXT     0x35
#define ATA_CMD_CACHE_FLUSH       0xE7
#define ATA_CMD_CACHE_FLUSH_EXT   0xEA
#define ATA_CMD_PACKET            0xA0
#define ATA_CMD_IDENTIFY_PACKET   0xA1
#define ATA_CMD_IDENTIFY          0xEC

/* Bus Master IDE Registers */
#define BMIDE_REG_COMMAND       0x00
#define BMIDE_REG_STATUS        0x02
#define BMIDE_REG_PRDT          0x04

#define BMIDE_CMD_START         0x01
#define BMIDE_CMD_READ          0x08    /* Read from disk (Write to memory) */

#define BMIDE_STATUS_INTERRUPT  0x04
#define BMIDE_STATUS_ERROR      0x02
#define BMIDE_STATUS_ACTIVE     0x01

/* PRDT Entry */
struct ide_prd {
  uint32_t addr;
  uint16_t size;
  uint16_t reserved: 15;
  uint16_t eot: 1; /* End of table */
}
    __packed;

struct ide_channel {
  uint16_t io_base;
  uint16_t ctrl_base;
  uint16_t bmide_base;
  uint8_t irq;
  bool nien;

  struct ide_device *devices[2]; /* 0: Master, 1: Slave */

  struct ide_prd *prdt;
  dma_addr_t prdt_phys;

  mutex_t lock;
  struct completion done;
  int error;
};

struct ide_device {
  struct block_device bdev;
  struct ide_channel *channel;
  uint8_t drive; /* 0: Master, 1: Slave */
  bool exists;
  bool atapi;
  bool lba48;
  uint64_t sectors;
  char model[41];
  char serial[21];
};

/* ATAPI Commands */
#define ATAPI_CMD_READ_10       0x28
#define ATAPI_CMD_READ_CAPACITY 0x25
#define ATAPI_CMD_EJECT         0x1B

struct ide_controller {
  struct pci_dev *pdev;
  struct ide_channel channels[2];
};

/* Internal API */
void ide_read_pio(struct ide_device *dev, uint64_t lba, uint32_t count, void *buf);

void ide_write_pio(struct ide_device *dev, uint64_t lba, uint32_t count, const void *buf);

int ide_atapi_read(struct ide_device *dev, uint32_t lba, uint32_t count, void *buf);

int ide_atapi_get_capacity(struct ide_device *ide);

int ide_read_dma(struct ide_device *dev, uint64_t lba, uint32_t count, void *buf);

int ide_write_dma(struct ide_device *dev, uint64_t lba, uint32_t count, const void *buf);

int ide_atapi_read_dma(struct ide_device *ide, uint32_t lba, uint32_t count, void *buf);

int ide_reset_channel(struct ide_channel *chan);

#define IDE_TIMEOUT_NS (5000000000ULL) /* 5 seconds */

/* Helper to wait for BSY to clear with timeout */
static inline int ide_wait_bsy(struct ide_channel *chan) {
  uint64_t timeout = get_time_ns() + IDE_TIMEOUT_NS;
  while (inb(chan->io_base + ATA_REG_STATUS) & ATA_SR_BSY) {
    if (get_time_ns() > timeout) return -ETIMEDOUT;
  }
  return 0;
}

/* Helper to wait for DRDY to set with timeout */
static inline int ide_wait_drdy(struct ide_channel *chan) {
  uint64_t timeout = get_time_ns() + IDE_TIMEOUT_NS;
  while (!(inb(chan->io_base + ATA_REG_STATUS) & ATA_SR_DRDY)) {
    if (get_time_ns() > timeout) return -ETIMEDOUT;
  }
  return 0;
}

/* Helper to wait for DRQ to set with timeout */
static inline int ide_wait_drq(struct ide_channel *chan) {
  uint64_t timeout = get_time_ns() + IDE_TIMEOUT_NS;
  while (!(inb(chan->io_base + ATA_REG_STATUS) & ATA_SR_DRQ)) {
    if (get_time_ns() > timeout) return -ETIMEDOUT;
  }
  return 0;
}
