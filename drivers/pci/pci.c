/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/pci/pci.c
 * @brief PCI Port I/O driver
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drivers/pci/pci.h>
#include <arch/x64/io.h>
#include <kernel/classes.h>
#include <kernel/fkx/fkx.h>
#include <lib/printk.h>
#include <kernel/sysintf/pci.h>

// Port I/O backend for PCI
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
      (UINT32_C(1) << 31) |
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

static void pci_enumerate_bus(uint8_t bus);

static void pci_check_device(uint8_t bus, uint8_t device) {
    pci_handle_t handle = {0, bus, device, 0};
    uint16_t vendor_id = (uint16_t)pci_read(&handle, 0, 16);
    if (vendor_id == 0xFFFF) return;

    // Check all functions for multi-function devices
    uint8_t header_type = (uint8_t)pci_read(&handle, 0x0E, 8);
    uint8_t func_count = (header_type & 0x80) ? 8 : 1;

    for (uint8_t func = 0; func < func_count; func++) {
        handle.function = func;
        vendor_id = (uint16_t)pci_read(&handle, 0, 16);
        if (vendor_id == 0xFFFF) continue;

        uint16_t device_id = (uint16_t)pci_read(&handle, 0x02, 16);
        uint8_t class_code = (uint8_t)pci_read(&handle, 0x0B, 8);
        uint8_t subclass = (uint8_t)pci_read(&handle, 0x0A, 8);

        printk(KERN_INFO PCI_CLASS "%02x:%02x.%d [%04x:%04x] class %02x subclass %02x\n",
               bus, device, func, vendor_id, device_id, class_code, subclass);

        // If it's a PCI-to-PCI bridge, enumerate the secondary bus
        if (class_code == 0x06 && subclass == 0x04) {
            uint8_t secondary_bus = (uint8_t)pci_read(&handle, 0x19, 8);
            pci_enumerate_bus(secondary_bus);
        }
    }
}

void pci_enumerate_bus(uint8_t bus) {
    for (uint8_t device = 0; device < 32; device++) {
        pci_check_device(bus, device);
    }
}
EXPORT_SYMBOL(pci_enumerate_bus);

static int pci_init() {
    printk( PCI_CLASS "Initializing PCI Port I/O\n");
    pci_register_ops(&pci_port_io_ops);
    return 0;
}

FKX_MODULE_DEFINE(
    pci,
    "1.0.0",
    "assembler-0",
    "PCI Bus Driver",
    0,
    FKX_DRIVER_CLASS,
    pci_init,
    NULL
);