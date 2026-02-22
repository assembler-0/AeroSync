/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/pci/backend_ecam.c
 * @brief PCI Express ECAM backend
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/pci.h>
#include <drivers/pci/pci.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <mm/vmalloc.h>

typedef struct {
  uint64_t phys_base;
  void *virt_base;
  uint16_t segment;
  uint8_t start_bus;
  uint8_t end_bus;
} ecam_region_t;

static ecam_region_t *regions = nullptr;
static int num_regions = 0;

static uint32_t pci_ecam_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  for (int i = 0; i < num_regions; i++) {
    if (regions[i].segment == p->segment && p->bus >= regions[i].start_bus &&
        p->bus <= regions[i].end_bus) {
      uint64_t addr =
          (uint64_t)regions[i].virt_base +
          (((uint64_t)p->bus - regions[i].start_bus) << 20 |
           (uint64_t)p->device << 15 | (uint64_t)p->function << 12 | offset);

      if (width == 8)
        return *(volatile uint8_t *)addr;
      if (width == 16)
        return *(volatile uint16_t *)addr;
      if (width == 32)
        return *(volatile uint32_t *)addr;
    }
  }
  return 0xFFFFFFFF;
}

static void pci_ecam_write(pci_handle_t *p, uint32_t offset, uint32_t val,
                           uint8_t width) {
  for (int i = 0; i < num_regions; i++) {
    if (regions[i].segment == p->segment && p->bus >= regions[i].start_bus &&
        p->bus <= regions[i].end_bus) {
      uint64_t addr =
          (uint64_t)regions[i].virt_base +
          (((uint64_t)p->bus - regions[i].start_bus) << 20 |
           (uint64_t)p->device << 15 | (uint64_t)p->function << 12 | offset);

      if (width == 8)
        *(volatile uint8_t *)addr = (uint8_t)val;
      else if (width == 16)
        *(volatile uint16_t *)addr = (uint16_t)val;
      else if (width == 32)
        *(volatile uint32_t *)addr = val;
      return;
    }
  }
}

static int pci_ecam_probe(void) { return num_regions > 0 ? 0 : -1; }

static pci_ops_t pci_ecam_ops = {
    .name = "ECAM",
    .read = pci_ecam_read,
    .write = pci_ecam_write,
    .probe = pci_ecam_probe,
    .priority = 100 // Higher than PIO
};

void pci_backend_ecam_init(void) {
  size_t entries_count = 0;
  const struct acpi_mcfg_allocation *entries =
      acpi_get_mcfg_entries(&entries_count);

  if (!entries || entries_count == 0) {
    return;
  }

  regions = kmalloc(sizeof(ecam_region_t) * entries_count);
  if (!regions) {
    printk(KERN_ERR PCI_CLASS "ECAM regions allocation error");
    return;
  }

  for (size_t i = 0; i < entries_count; i++) {
    const struct acpi_mcfg_allocation *alloc = &entries[i];
    regions[i].phys_base = alloc->Address;
    regions[i].segment = alloc->PciSegment;
    regions[i].start_bus = alloc->StartBusNumber;
    regions[i].end_bus = alloc->EndBusNumber;

    size_t size = (size_t)(alloc->EndBusNumber - alloc->StartBusNumber + 1) << 20;
    regions[i].virt_base = ioremap(alloc->Address, size);

    printk(KERN_DEBUG PCI_CLASS "ECAM Segment %d Bus %02x-%02x mapped at %p\n",
           alloc->PciSegment, alloc->StartBusNumber, alloc->EndBusNumber,
           regions[i].virt_base);
  }

  num_regions = (int)entries_count;
  pci_register_ops(&pci_ecam_ops);
}
