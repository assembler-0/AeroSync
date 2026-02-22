/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/dmar.c
 * @brief Generic DMAR Table Parser Implementation using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <aerosync/sysintf/dmar.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <lib/printk.h>
#include <mm/slub.h>

static LIST_HEAD(s_dmar_units);
static LIST_HEAD(s_dmar_reserved_regions);
static LIST_HEAD(s_dmar_atsr_units);

/**
 * @brief Parse device scope entries within DMAR subtables
 */
static void parse_device_scope(struct list_head *dev_list, void *scope_start, size_t total_len) {
  uint8_t *curr = (uint8_t *)scope_start;
  uint8_t *end = curr + total_len;

  while (curr < end) {
    dmar_device_scope_t *scope = (dmar_device_scope_t *)curr;
    if (scope->length < sizeof(dmar_device_scope_t)) break;

    /* 0x01: PCI Endpoint, 0x02: PCI Sub-hierarchy */
    if (scope->type == 0x01 || scope->type == 0x02) {
      size_t path_len = scope->length - offsetof(dmar_device_scope_t, path);
      size_t num_paths = path_len / sizeof(scope->path[0]);
      
      uint8_t bus = scope->start_bus;
      for (size_t i = 0; i < num_paths; i++) {
        dmar_dev_t *ddev = (dmar_dev_t *)kmalloc(sizeof(dmar_dev_t));
        if (ddev) {
          ddev->bus = bus;
          ddev->devfn = (scope->path[i].device << 3) | scope->path[i].function;
          list_add_tail(&ddev->node, dev_list);
          printk(KERN_DEBUG ACPI_CLASS "  Device scope: %02x:%02x.%x\n", 
                 bus, scope->path[i].device, scope->path[i].function);
        }
      }
    }
    curr += scope->length;
  }
}

static int dmar_parse_subtables(ACPI_TABLE_DMAR *hdr) {
  uint8_t *cursor = (uint8_t *)hdr + sizeof(ACPI_TABLE_DMAR);
  size_t bytes_left = hdr->Header.Length - sizeof(ACPI_TABLE_DMAR);

  while (bytes_left >= sizeof(ACPI_DMAR_HEADER)) {
    ACPI_DMAR_HEADER *sub_hdr = (ACPI_DMAR_HEADER *)cursor;
    if (sub_hdr->Length < sizeof(ACPI_DMAR_HEADER) || sub_hdr->Length > bytes_left) {
      printk(KERN_ERR ACPI_CLASS "DMAR: corrupted subtable length %u (%zu bytes left)\n",
             sub_hdr->Length, bytes_left);
      return -EINVAL;
    }

    switch (sub_hdr->Type) {
      case ACPI_DMAR_TYPE_HARDWARE_UNIT: {
        ACPI_DMAR_HARDWARE_UNIT *drhd = (ACPI_DMAR_HARDWARE_UNIT *)cursor;
        dmar_unit_t *unit = (dmar_unit_t *)kmalloc(sizeof(dmar_unit_t));
        if (unit) {
          unit->segment = drhd->Segment;
          unit->address = drhd->Address;
          unit->flags = drhd->Flags;
          INIT_LIST_HEAD(&unit->devices);
          list_add_tail(&unit->node, &s_dmar_units);

          printk(KERN_DEBUG ACPI_CLASS "IOMMU (DRHD): segment=%u, addr=0x%llx, flags=0x%x\n",
                 unit->segment, unit->address, unit->flags);

          parse_device_scope(&unit->devices, (void *)(drhd + 1),
                             drhd->Header.Length - sizeof(ACPI_DMAR_HARDWARE_UNIT));
        }
        break;
      }
      case ACPI_DMAR_TYPE_RESERVED_MEMORY: {
        ACPI_DMAR_RESERVED_MEMORY *rmrr = (ACPI_DMAR_RESERVED_MEMORY *)cursor;
        dmar_reserved_region_t *region = (dmar_reserved_region_t *)kmalloc(sizeof(dmar_reserved_region_t));
        if (region) {
          region->segment = rmrr->Segment;
          region->base_address = rmrr->BaseAddress;
          region->end_address = rmrr->EndAddress;
          INIT_LIST_HEAD(&region->devices);
          list_add_tail(&region->node, &s_dmar_reserved_regions);

          printk(KERN_DEBUG ACPI_CLASS "Reserved Region (RMRR): base=0x%llx, end=0x%llx\n",
                 region->base_address, region->end_address);

          parse_device_scope(&region->devices, (void *)(rmrr + 1),
                             rmrr->Header.Length - sizeof(ACPI_DMAR_RESERVED_MEMORY));
        }
        break;
      }
      case ACPI_DMAR_TYPE_ROOT_ATS: {
        ACPI_DMAR_ATSR *atsr = (ACPI_DMAR_ATSR *)cursor;
        dmar_atsr_t *atsru = (dmar_atsr_t *)kmalloc(sizeof(dmar_atsr_t));
        if (atsru) {
          atsru->segment = atsr->Segment;
          atsru->flags = atsr->Flags;
          INIT_LIST_HEAD(&atsru->devices);
          list_add_tail(&atsru->node, &s_dmar_atsr_units);

          printk(KERN_DEBUG ACPI_CLASS "ATS Reporting (ATSR): segment=%u, flags=0x%x\n",
                 atsru->segment, atsru->flags);

          parse_device_scope(&atsru->devices, (void *)(atsr + 1),
                             atsr->Header.Length - sizeof(ACPI_DMAR_ATSR));
        }
        break;
      }
      default:
        printk(KERN_NOTICE ACPI_CLASS "DMAR: subtable type %u length %u ignored\n",
               sub_hdr->Type, sub_hdr->Length);
        break;
    }

    cursor += sub_hdr->Length;
    bytes_left -= sub_hdr->Length;
  }

  return 0;
}

int dmar_init(void) {
  ACPI_TABLE_DMAR *dmar;
  ACPI_STATUS status = AcpiGetTable(ACPI_SIG_DMAR, 1, (ACPI_TABLE_HEADER **)&dmar);
  if (ACPI_FAILURE(status)) {
    return -ENODEV;
  }

  printk(KERN_INFO ACPI_CLASS "Parsing DMAR (DMA Remapping Reporting Table)...\n");
  dmar_parse_subtables(dmar);

  return 0;
}

struct list_head *dmar_get_units(void) { return &s_dmar_units; }
struct list_head *dmar_get_reserved_regions(void) { return &s_dmar_reserved_regions; }
struct list_head *dmar_get_atsr_units(void) { return &s_dmar_atsr_units; }

#include <aerosync/export.h>
EXPORT_SYMBOL(dmar_init);
EXPORT_SYMBOL(dmar_get_units);
EXPORT_SYMBOL(dmar_get_reserved_regions);
EXPORT_SYMBOL(dmar_get_atsr_units);
