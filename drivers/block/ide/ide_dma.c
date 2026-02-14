/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/block/ide/ide_dma.c
 * @brief DMA Backend for IDE Driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include <drivers/block/ide/ide.h>
#include <arch/x86_64/io.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/errno.h>

static int ide_do_dma(struct ide_device* ide, uint64_t lba, uint32_t count, void* buf, bool is_write) {
  struct ide_channel* chan = ide->channel;
  if (!chan->bmide_base) return -ENOSYS;

  mutex_lock(&chan->lock);

  /* Map buffer for DMA */
  dma_addr_t phys = dma_map_single(&ide->bdev.dev, buf, count * 512, is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

  /* Setup PRDT (one entry for simplicity) */
  chan->prdt[0].addr = (uint32_t)phys;
  chan->prdt[0].size = (uint16_t)(count * 512); /* 0 means 64KB */
  chan->prdt[0].eot = 1;

  /* Set PRDT address */
  outl(chan->bmide_base + BMIDE_REG_PRDT, (uint32_t)chan->prdt_phys);

  /* Clear Error and Interrupt bits in Status register */
  outb(chan->bmide_base + BMIDE_REG_STATUS, inb(chan->bmide_base + BMIDE_REG_STATUS) | 0x06);

  /* Set Direction in Command register */
  uint8_t cmd = is_write ? 0 : BMIDE_CMD_READ;
  outb(chan->bmide_base + BMIDE_REG_COMMAND, cmd);

  /* Prepare the drive */
  ide_wait_bsy(chan);
  outb(chan->io_base + ATA_REG_DRIVE, 0xE0 | (ide->drive << 4) | ((lba >> 24) & 0x0F));

  /* Sector Count */
  outb(chan->io_base + ATA_REG_SEC_COUNT, (uint8_t)count);

  /* LBA Low, Mid, High */
  outb(chan->io_base + ATA_REG_LBA_LOW, (uint8_t)lba);
  outb(chan->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
  outb(chan->io_base + ATA_REG_LBA_HIGH, (uint8_t)(lba >> 16));

  /* Issue DMA command */
  reinit_completion(&chan->done);
  outb(chan->io_base + ATA_REG_COMMAND, is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA);

  /* Start Bus Master DMA */
  outb(chan->bmide_base + BMIDE_REG_COMMAND, cmd | BMIDE_CMD_START);

  /* Wait for completion (interrupt) */
  wait_for_completion(&chan->done);

  /* Stop DMA */
  outb(chan->bmide_base + BMIDE_REG_COMMAND, cmd);

  /* Check status */
  uint8_t status = inb(chan->bmide_base + BMIDE_REG_STATUS);
  int ret = 0;
  if (status & BMIDE_STATUS_ERROR) ret = -EIO;
  if (chan->error) ret = -EIO;

  /* Unmap buffer */
  dma_unmap_single(&ide->bdev.dev, phys, count * 512, is_write ? DMA_TO_DEVICE : DMA_FROM_DEVICE);

  mutex_unlock(&chan->lock);
  return ret;
}

int ide_read_dma(struct ide_device* ide, uint64_t lba, uint32_t count, void* buf) {
  return ide_do_dma(ide, lba, count, buf, false);
}

int ide_write_dma(struct ide_device* ide, uint64_t lba, uint32_t count, const void* buf) {
  return ide_do_dma(ide, lba, (uint32_t)count, (void*)buf, true);
}

int ide_atapi_read_dma(struct ide_device* ide, uint32_t lba, uint32_t count, void* buf) {
  struct ide_channel* chan = ide->channel;
  if (!chan->bmide_base) return -ENOSYS;

  mutex_lock(&chan->lock);

  size_t byte_count = count * 2048;
  dma_addr_t phys = dma_map_single(&ide->bdev.dev, buf, byte_count, DMA_FROM_DEVICE);

  /* Setup PRDT */
  chan->prdt[0].addr = (uint32_t)phys;
  chan->prdt[0].size = (uint16_t)byte_count;
  chan->prdt[0].eot = 1;

  outl(chan->bmide_base + BMIDE_REG_PRDT, (uint32_t)chan->prdt_phys);
  outb(chan->bmide_base + BMIDE_REG_STATUS, inb(chan->bmide_base + BMIDE_REG_STATUS) | 0x06);
  outb(chan->bmide_base + BMIDE_REG_COMMAND, BMIDE_CMD_READ);

  /* Prepare the drive for ATAPI DMA */
  ide_wait_bsy(chan);
  outb(chan->io_base + ATA_REG_DRIVE, ide->drive << 4);
  ide_wait_bsy(chan);

  /* Features: bit 0 = DMA, bit 1 = Overlap (not used) */
  outb(chan->io_base + ATA_REG_FEATURES, 0x01);

  /* Byte count limit (ignored by most drives in DMA mode but set for safety) */
  outb(chan->io_base + ATA_REG_LBA_MID, 0x00);
  outb(chan->io_base + ATA_REG_LBA_HIGH, 0x08);

  /* Send PACKET command */
  outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_PACKET);
  ide_wait_bsy(chan);

  /* Wait for DRQ to send packet */
  uint8_t status;
  while (!((status = inb(chan->io_base + ATA_REG_STATUS)) & ATA_SR_DRQ)) {
    if (status & ATA_SR_ERR) {
      dma_unmap_single(&ide->bdev.dev, phys, byte_count, DMA_FROM_DEVICE);
      mutex_unlock(&chan->lock);
      return -EIO;
    }
  }

  /* Send the 12-byte SCSI Packet */
  uint8_t packet[12] = {0};
  packet[0] = ATAPI_CMD_READ_10;
  packet[2] = (lba >> 24) & 0xFF;
  packet[3] = (lba >> 16) & 0xFF;
  packet[4] = (lba >> 8) & 0xFF;
  packet[5] = lba & 0xFF;
  packet[7] = (count >> 8) & 0xFF;
  packet[8] = count & 0xFF;

  outsw(chan->io_base + ATA_REG_DATA, (uint16_t*)packet, 6);

  /* Start DMA */
  reinit_completion(&chan->done);
  outb(chan->bmide_base + BMIDE_REG_COMMAND, BMIDE_CMD_READ | BMIDE_CMD_START);

  /* Wait for completion */
  wait_for_completion(&chan->done);

  /* Stop DMA */
  outb(chan->bmide_base + BMIDE_REG_COMMAND, BMIDE_CMD_READ);

  uint8_t bm_status = inb(chan->bmide_base + BMIDE_REG_STATUS);
  int ret = 0;
  if (bm_status & BMIDE_STATUS_ERROR) ret = -EIO;
  if (chan->error) ret = -EIO;

  dma_unmap_single(&ide->bdev.dev, phys, byte_count, DMA_FROM_DEVICE);
  mutex_unlock(&chan->lock);

  return ret;
}
