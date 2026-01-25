/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/block/ide/ide_pio.c
 * @brief PIO Backend for IDE Driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include "ide.h"
#include <arch/x86_64/io.h>

void ide_read_pio(struct ide_device *ide, uint64_t lba, uint32_t count, void *buf) {
  struct ide_channel *chan = ide->channel;
  uint16_t *ptr = (uint16_t *) buf;

  mutex_lock(&chan->lock);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t curr_lba = lba + i;

    ide_wait_bsy(chan);

    /* Select drive and LBA 24-27 */
    outb(chan->io_base + ATA_REG_DRIVE, 0xE0 | (ide->drive << 4) | ((curr_lba >> 24) & 0x0F));

    /* Sector Count */
    outb(chan->io_base + ATA_REG_SEC_COUNT, 1);

    /* LBA Low, Mid, High */
    outb(chan->io_base + ATA_REG_LBA_LOW, (uint8_t) curr_lba);
    outb(chan->io_base + ATA_REG_LBA_MID, (uint8_t) (curr_lba >> 8));
    outb(chan->io_base + ATA_REG_LBA_HIGH, (uint8_t) (curr_lba >> 16));

    /* Command: READ PIO */
    outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    /* Wait for drive to be ready to transfer data */
    ide_wait_bsy(chan);
    ide_wait_drdy(chan);

    /* Read 256 words (512 bytes) */
    insw(chan->io_base + ATA_REG_DATA, ptr, 256);
    ptr += 256;
  }

  mutex_unlock(&chan->lock);
}

void ide_write_pio(struct ide_device *ide, uint64_t lba, uint32_t count, const void *buf) {
  struct ide_channel *chan = ide->channel;
  uint16_t *ptr = (uint16_t *) buf;

  mutex_lock(&chan->lock);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t curr_lba = lba + i;

    ide_wait_bsy(chan);

    outb(chan->io_base + ATA_REG_DRIVE, 0xE0 | (ide->drive << 4) | ((curr_lba >> 24) & 0x0F));
    outb(chan->io_base + ATA_REG_SEC_COUNT, 1);
    outb(chan->io_base + ATA_REG_LBA_LOW, (uint8_t) curr_lba);
    outb(chan->io_base + ATA_REG_LBA_MID, (uint8_t) (curr_lba >> 8));
    outb(chan->io_base + ATA_REG_LBA_HIGH, (uint8_t) (curr_lba >> 16));

    /* Command: WRITE PIO */
    outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    ide_wait_bsy(chan);
    ide_wait_drdy(chan);

    /* Write 256 words */
    outsw(chan->io_base + ATA_REG_DATA, ptr, 256);
    ptr += 256;

    /* Flush cache after each sector for safety in this simple impl */
    outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ide_wait_bsy(chan);
  }

  mutex_unlock(&chan->lock);
}
