/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/block/ide/ide.c
 * @brief IDE/ATA Block Driver Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/pci.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/errno.h>
#include <aerosync/classes.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/irq.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <lib/string.h>
#include <mm/slub.h>

#include "ide.h"

static struct ide_controller *g_ide_ctrl = nullptr;

static void ide_irq_handler(cpu_regs *regs) {
  uint8_t irq = regs->interrupt_number;
  struct ide_channel *chan = nullptr;

  if (!g_ide_ctrl) return;

  if (irq == g_ide_ctrl->channels[0].irq) {
    chan = &g_ide_ctrl->channels[0];
  } else if (irq == g_ide_ctrl->channels[1].irq) {
    chan = &g_ide_ctrl->channels[1];
  }

  if (chan) {
    /* Read status to acknowledge interrupt */
    chan->error = inb(chan->io_base + ATA_REG_STATUS) & ATA_SR_ERR;
    complete(&chan->done);
  }
}

static void ide_select_drive(struct ide_channel *chan, uint8_t drive) {
  outb(chan->io_base + ATA_REG_DRIVE, 0xA0 | (drive << 4));
  /* Wait a bit for the drive to respond */
  for (int i = 0; i < 4; i++) inb(chan->ctrl_base + ATA_REG_ALT_STATUS);
}

static int ide_identify(struct ide_device *ide) {
  struct ide_channel *chan = ide->channel;

  ide_select_drive(chan, ide->drive);

  outb(chan->io_base + ATA_REG_SEC_COUNT, 0);
  outb(chan->io_base + ATA_REG_LBA_LOW, 0);
  outb(chan->io_base + ATA_REG_LBA_MID, 0);
  outb(chan->io_base + ATA_REG_LBA_HIGH, 0);

  outb(chan->io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

  uint8_t status = inb(chan->io_base + ATA_REG_STATUS);
  if (status == 0) return -ENODEV; /* No device */

  ide_wait_bsy(chan);

  /* Check if it's ATAPI */
  if (inb(chan->io_base + ATA_REG_LBA_MID) != 0 || inb(chan->io_base + ATA_REG_LBA_HIGH) != 0) {
    /* This is an ATAPI device, we don't support it yet */
    return -ENOSYS;
  }

  /* Wait for DRQ or ERR */
  while (1) {
    status = inb(chan->io_base + ATA_REG_STATUS);
    if (status & ATA_SR_ERR) return -EIO;
    if (status & ATA_SR_DRQ) break;
  }

  uint16_t data[256];
  insw(chan->io_base + ATA_REG_DATA, data, 256);

  /* Parse IDENTIFY data */
  ide->lba48 = (data[83] & (1 << 10)) != 0;
  if (ide->lba48) {
    ide->sectors = *((uint64_t *) &data[100]);
  } else {
    ide->sectors = *((uint32_t *) &data[60]);
  }

  /* Model Name */
  for (int i = 0; i < 20; i++) {
    ide->model[i * 2] = data[27 + i] >> 8;
    ide->model[i * 2 + 1] = data[27 + i] & 0xFF;
  }
  ide->model[40] = '\0';

  ide->exists = true;
  return 0;
}

static int ide_read(struct block_device *bdev, void *buffer, uint64_t start_sector, uint32_t sector_count) {
  struct ide_device *ide = (struct ide_device *) bdev;

  /* Prefer DMA, fallback to PIO */
  int ret = ide_read_dma(ide, start_sector, sector_count, buffer);
  if (ret == -ENOSYS || ret == -EIO) {
    ide_read_pio(ide, start_sector, sector_count, buffer);
    ret = 0;
  }

  return ret;
}

static int ide_write(struct block_device *bdev, const void *buffer, uint64_t start_sector, uint32_t sector_count) {
  struct ide_device *ide = (struct ide_device *) bdev;

  int ret = ide_write_dma(ide, start_sector, sector_count, (void *) buffer);
  if (ret == -ENOSYS || ret == -EIO) {
    ide_write_pio(ide, start_sector, sector_count, buffer);
    ret = 0;
  }

  return ret;
}

static const struct block_operations ide_ops = {
  .read = ide_read,
  .write = ide_write,
};

static int ide_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
  printk(KERN_INFO ATA_CLASS "Probing IDE Controller at %02x:%02x.%d\n",
         pdev->handle.bus, pdev->handle.device, pdev->handle.function);

  struct ide_controller *ctrl = kmalloc(sizeof(struct ide_controller));
  memset(ctrl, 0, sizeof(struct ide_controller));
  ctrl->pdev = pdev;
  g_ide_ctrl = ctrl;

  uint8_t prog_if = pci_read_config8(pdev, PCI_PROG_IF);

  /* Primary Channel */
  if (prog_if & 0x01) {
    ctrl->channels[0].io_base = pdev->bars[0] & ~1;
    ctrl->channels[0].ctrl_base = pdev->bars[1] & ~1;
    /* Native mode IRQ usually from PCI config */
    ctrl->channels[0].irq = pci_read_config8(pdev, 0x3C);
  } else {
    ctrl->channels[0].io_base = 0x1F0;
    ctrl->channels[0].ctrl_base = 0x3F6;
    ctrl->channels[0].irq = 14;
  }

  /* Secondary Channel */
  if (prog_if & 0x04) {
    ctrl->channels[1].io_base = pdev->bars[2] & ~1;
    ctrl->channels[1].ctrl_base = pdev->bars[3] & ~1;
    ctrl->channels[1].irq = pci_read_config8(pdev, 0x3C);
  } else {
    ctrl->channels[1].io_base = 0x170;
    ctrl->channels[1].ctrl_base = 0x376;
    ctrl->channels[1].irq = 15;
  }

  /* Bus Master IDE (DMA) */
  uint16_t bmide = pdev->bars[4] & ~1;
  if (bmide) {
    ctrl->channels[0].bmide_base = bmide;
    ctrl->channels[1].bmide_base = bmide + 8;
    pci_enable_device(pdev);
    pci_set_master(pdev);
  }

  for (int i = 0; i < 2; i++) {
    struct ide_channel *chan = &ctrl->channels[i];

    /* Allocate PRDT (one page is enough for many entries, but we only need one for now) */
    chan->prdt = dma_alloc_coherent(4096, &chan->prdt_phys, GFP_KERNEL);
    memset(chan->prdt, 0, 4096);

    mutex_init(&chan->lock);
    init_completion(&chan->done);

    /* Register IRQ */
    irq_install_handler(chan->irq, ide_irq_handler);
    ic_enable_irq(chan->irq);

    /* Disable IRQs during discovery */
    outb(chan->ctrl_base + ATA_REG_CONTROL, 0x02);

    for (int d = 0; d < 2; d++) {
      struct ide_device *ide = kmalloc(sizeof(struct ide_device));
      memset(ide, 0, sizeof(struct ide_device));
      ide->channel = chan;
      ide->drive = d;

      if (ide_identify(ide) == 0) {
        chan->devices[d] = ide;
        snprintf(ide->bdev.name, BLOCK_NAME_MAX, "hd%c", 'a' + (i * 2 + d));
        ide->bdev.block_size = 512;
        ide->bdev.sector_count = ide->sectors;
        ide->bdev.ops = &ide_ops;
        ide->bdev.private_data = ide;

        block_device_register(&ide->bdev);

        printk(KERN_INFO ATA_CLASS "Found %s: %s (%llu MB)\n",
               ide->bdev.name, ide->model, (ide->sectors * 512) / 1024 / 1024);
      } else {
        kfree(ide);
      }
    }

    /* Re-enable IRQs */
    outb(chan->ctrl_base + ATA_REG_CONTROL, 0x00);
  }

  return 0;
}

static struct pci_device_id ide_pci_ids[] = {
  {.class = 0x010100, .class_mask = 0xFFFF00}, /* IDE Controller */
  {0}
};

static struct pci_driver ide_pci_driver = {
  .name = "ide",
  .id_table = ide_pci_ids,
  .probe = ide_probe,
};

static int ide_init(void) {
  return pci_register_driver(&ide_pci_driver);
}

const char *dependency_names[] = {"pci", nullptr};

FKX_MODULE_DEFINE(
  ide,
  "0.0.2",
  "assembler-0",
  "Standard IDE/ATA Block Driver",
  0,
  FKX_DRIVER_CLASS,
  ide_init,
  dependency_names
);
