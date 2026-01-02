/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/pci/backend_pio.c
 * @brief PCI Port I/O backend
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/types.h>
#include <arch/x86_64/io.h>
#include <kernel/sysintf/pci.h>
#include <lib/printk.h>

static uint32_t pci_port_io_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  if (p->segment != 0) return 0xFFFFFFFF;

  uint32_t address =
      (1u << 31) |
      ((uint32_t)p->bus     << 16) |
      ((uint32_t)p->device  << 11) |
      ((uint32_t)p->function<< 8)  |
      (offset & 0xFC);

  outl(0xCF8, address);
  uint32_t val = inl(0xCFC);

  uint32_t shift = (offset & 3) * 8;

  if (width == 8) return (val >> shift) & 0xFF;
  if (width == 16) return (val >> shift) & 0xFFFF;
  return val;
}

static void pci_port_io_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
  if (p->segment != 0) return;

  uint32_t address =
      (1u << 31) |
      ((uint32_t)p->bus     << 16) |
      ((uint32_t)p->device  << 11) |
      ((uint32_t)p->function<< 8)  |
      ((uint32_t)offset & 0xFC);

  outl(0xCF8, address);

  if (width != 32) {
    uint32_t current_val = inl(0xCFC);
    uint32_t shift = (offset & 3) * 8;
    uint32_t mask = ((1ULL << width) - 1) << shift;
    current_val &= ~mask;
    current_val |= (val << shift);
    outl(0xCFC, current_val);
  } else {
    outl(0xCFC, val);
  }
}

static int pci_port_io_probe(void) {
    // Port I/O is always available on x86
    return 0;
}

static pci_ops_t pci_port_io_ops = {
    .name = "Port I/O",
    .read = pci_port_io_read,
    .write = pci_port_io_write,
    .probe = pci_port_io_probe,
    .priority = 10
};

void pci_backend_pio_init(void) {
    pci_register_ops(&pci_port_io_ops);
}
