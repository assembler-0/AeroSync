/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/block/ide/ide_pio.c
 * @brief PIO Backend for IDE Driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/errno.h>
#include <drivers/block/ide/ide.h>
#include <arch/x86_64/io.h>

void ide_read_pio(struct ide_device *ide, uint64_t lba, uint32_t count, void *buf) {
  struct ide_channel *chan = ide->channel;
  uint16_t *ptr = (uint16_t *) buf;

  mutex_lock(&chan->lock);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t curr_lba = lba + i;

    if (ide_wait_bsy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return; /* Should probably return int error but PIO read is void for now */
    }

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
    if (ide_wait_bsy(chan) != 0 || ide_wait_drdy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return;
    }

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

    if (ide_wait_bsy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return;
    }

    outb(chan->io_base + ATA_REG_DRIVE, 0xE0 | (ide->drive << 4) | ((curr_lba >> 24) & 0x0F));
    outb(chan->io_base + ATA_REG_SEC_COUNT, 1);
    outb(chan->io_base + ATA_REG_LBA_LOW, (uint8_t) curr_lba);
    outb(chan->io_base + ATA_REG_LBA_MID, (uint8_t) (curr_lba >> 8));
    outb(chan->io_base + ATA_REG_LBA_HIGH, (uint8_t) (curr_lba >> 16));

    /* Command: WRITE PIO */
    outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    if (ide_wait_bsy(chan) != 0 || ide_wait_drdy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return;
    }

    /* Write 256 words */
    outsw(chan->io_base + ATA_REG_DATA, ptr, 256);
    ptr += 256;

    /* Flush cache after each sector for safety in this simple impl */
    outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    if (ide_wait_bsy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return;
    }
  }

  mutex_unlock(&chan->lock);
}

int ide_atapi_read(struct ide_device *ide, uint32_t lba, uint32_t count, void *buf) {
  struct ide_channel *chan = ide->channel;
  uint16_t *ptr = (uint16_t *) buf;

  mutex_lock(&chan->lock);

  /* 1. Select Drive */
  outb(chan->io_base + ATA_REG_DRIVE, ide->drive << 4);
  if (ide_wait_bsy(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* 2. Set Features to 0 (PIO mode) and Byte Count to 2048 (Standard CD-ROM sector) */
  outb(chan->io_base + ATA_REG_FEATURES, 0);
  outb(chan->io_base + ATA_REG_LBA_MID, 0x00);
  outb(chan->io_base + ATA_REG_LBA_HIGH, 0x08); /* 2048 bytes */

  /* 3. Send PACKET command */
  outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);
  if (ide_wait_bsy(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* Wait for DRQ to send the packet */
  if (ide_wait_drq(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* 4. Send the 12-byte ATAPI Packet (READ 10) */
  uint8_t packet[12] = {0};
  packet[0] = ATAPI_CMD_READ_10;
  packet[2] = (lba >> 24) & 0xFF;
  packet[3] = (lba >> 16) & 0xFF;
  packet[4] = (lba >> 8) & 0xFF;
  packet[5] = lba & 0xFF;
  packet[7] = (count >> 8) & 0xFF;
  packet[8] = count & 0xFF;

  outsw(chan->io_base + ATA_REG_DATA, (uint16_t *) packet, 6);

  /* 5. Read the data */
  for (uint32_t i = 0; i < count; i++) {
    if (ide_wait_bsy(chan) != 0) {
      mutex_unlock(&chan->lock);
      return -ETIMEDOUT;
    }
    if (ide_wait_drq(chan) != 0) {
      mutex_unlock(&chan->lock);
      return -ETIMEDOUT;
    }

    /* Read one sector (usually 2048 bytes for ATAPI) */
    insw(chan->io_base + ATA_REG_DATA, ptr, 1024);
    ptr += 1024;
  }
  mutex_unlock(&chan->lock);
  return 0;
}


int ide_atapi_get_capacity(struct ide_device *ide) {
  struct ide_channel *chan = ide->channel;
  uint8_t packet[12] = {0};
  uint8_t response[8] = {0};
  uint8_t status;

  mutex_lock(&chan->lock);

  /* 1. Select Drive */
  outb(chan->io_base + ATA_REG_DRIVE, ide->drive << 4);
  if (ide_wait_bsy(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* 2. Set Features to 0 and Byte Count to 8 */
  outb(chan->io_base + ATA_REG_FEATURES, 0);
  outb(chan->io_base + ATA_REG_LBA_MID, 8);
  outb(chan->io_base + ATA_REG_LBA_HIGH, 0);

  /* 3. Send PACKET command */
  outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);
  if (ide_wait_bsy(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  if (ide_wait_drq(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* 4. Send the 12-byte ATAPI Packet (READ CAPACITY) */
  packet[0] = ATAPI_CMD_READ_CAPACITY;
  outsw(chan->io_base + ATA_REG_DATA, (uint16_t *) packet, 6);

  if (ide_wait_bsy(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }
  if (ide_wait_drq(chan) != 0) {
    mutex_unlock(&chan->lock);
    return -ETIMEDOUT;
  }

  /* 5. Read 8 bytes of response */
  insw(chan->io_base + ATA_REG_DATA, (uint16_t *) response, 4);

  uint32_t last_lba = (response[0] << 24) | (response[1] << 16) | (response[2] << 8) | response[3];
  uint32_t block_len = (response[4] << 24) | (response[5] << 16) | (response[6] << 8) | response[7];

  if (block_len > 0) {
    ide->sectors = (uint64_t) last_lba + 1;
    ide->bdev.block_size = block_len;
  } else {
    /* Default fallback for some emulators/drives */
    ide->sectors = 0;
    ide->bdev.block_size = 2048;
  }

  mutex_unlock(&chan->lock);
  return 0;
}
