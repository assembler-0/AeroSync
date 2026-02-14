/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/dmar.c
 * @brief Generic DMAR Table Parser Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/dmar.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <lib/printk.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <mm/slub.h>
#include <lib/string.h>

#define ACPI_SIG_DMAR "DMAR"

struct acpi_dmar_header {
  struct acpi_sdt_hdr hdr;
  uint8_t width;
  uint8_t flags;
  uint8_t reserved[10];
} __packed;

struct acpi_dmar_entry_header {
  uint16_t type;
  uint16_t length;
} __packed;

struct acpi_dmar_drhd_header {
  struct acpi_dmar_entry_header entry;
  uint8_t flags;
  uint8_t reserved;
  uint16_t segment;
  uint64_t address;
} __packed;

struct acpi_dmar_rmrr_header {
  struct acpi_dmar_entry_header entry;
  uint16_t reserved;
  uint16_t segment;
  uint64_t base_address;
  uint64_t end_address;
} __packed;

struct acpi_dmar_atsr_header {
  struct acpi_dmar_entry_header entry;
  uint8_t flags;
  uint8_t reserved;
  uint16_t segment;
} __packed;

static LIST_HEAD(s_dmar_units);
static LIST_HEAD(s_dmar_reserved_regions);
static LIST_HEAD(s_dmar_atsr_units);

/**
 * @brief Parse device scope entries within DMAR subtables
 */
static void parse_device_scope(struct list_head *dev_list, void *scope_start, size_t total_len) {
  auto curr = (uint8_t *)scope_start;
  auto end = curr + total_len;

  while (curr < end) {
    auto scope = (dmar_device_scope_t *)curr;
    if (scope->length < sizeof(dmar_device_scope_t)) break;

    /* 0x01: PCI Endpoint, 0x02: PCI Sub-hierarchy */
    if (scope->type == 0x01 || scope->type == 0x02) {
      auto path_len = scope->length - offsetof(dmar_device_scope_t, path);
      auto num_paths = path_len / sizeof(scope->path[0]);
      
      auto bus = scope->start_bus;
      for (size_t i = 0; i < num_paths; i++) {
        auto ddev = (dmar_dev_t *)kmalloc(sizeof(dmar_dev_t));
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

static int dmar_parse_subtables(struct acpi_dmar_header *hdr) {
  auto cursor = (uint8_t *)hdr + sizeof(struct acpi_dmar_header);
  size_t bytes_left = hdr->hdr.length - sizeof(struct acpi_dmar_header);

  while (bytes_left >= sizeof(struct acpi_dmar_entry_header)) {
    auto sub_hdr = (struct acpi_dmar_entry_header *)cursor;
    if (sub_hdr->length < sizeof(struct acpi_dmar_entry_header) || sub_hdr->length > bytes_left) {
      printk(KERN_ERR ACPI_CLASS "DMAR: corrupted subtable length %u (%zu bytes left)\n",
             sub_hdr->length, bytes_left);
      return -EINVAL;
    }

    switch (sub_hdr->type) {
      case DMAR_TYPE_DRHD: {
        auto drhd_acpi = (struct acpi_dmar_drhd_header *)cursor;
        auto unit = (dmar_unit_t *)kmalloc(sizeof(dmar_unit_t));
        if (unit) {
          unit->segment = drhd_acpi->segment;
          unit->address = drhd_acpi->address;
          unit->flags = drhd_acpi->flags;
          INIT_LIST_HEAD(&unit->devices);
          list_add_tail(&unit->node, &s_dmar_units);

          printk(KERN_DEBUG ACPI_CLASS "IOMMU (DRHD): segment=%u, addr=0x%llx, flags=0x%x\n",
                 unit->segment, unit->address, unit->flags);

          parse_device_scope(&unit->devices, (void *)(drhd_acpi + 1),
                             drhd_acpi->entry.length - sizeof(struct acpi_dmar_drhd_header));
        }
        break;
      }
      case DMAR_TYPE_RMRR: {
        auto rmrr_acpi = (struct acpi_dmar_rmrr_header *)cursor;
        auto region = (dmar_reserved_region_t *)kmalloc(sizeof(dmar_reserved_region_t));
        if (region) {
          region->segment = rmrr_acpi->segment;
          region->base_address = rmrr_acpi->base_address;
          region->end_address = rmrr_acpi->end_address;
          INIT_LIST_HEAD(&region->devices);
          list_add_tail(&region->node, &s_dmar_reserved_regions);

          printk(KERN_DEBUG ACPI_CLASS "Reserved Region (RMRR): base=0x%llx, end=0x%llx\n",
                 region->base_address, region->end_address);

          parse_device_scope(&region->devices, (void *)(rmrr_acpi + 1),
                             rmrr_acpi->entry.length - sizeof(struct acpi_dmar_rmrr_header));
        }
        break;
      }
      case DMAR_TYPE_ATSR: {
        auto atsr_acpi = (struct acpi_dmar_atsr_header *)cursor;
        auto atsr = (dmar_atsr_t *)kmalloc(sizeof(dmar_atsr_t));
        if (atsr) {
          atsr->segment = atsr_acpi->segment;
          atsr->flags = atsr_acpi->flags;
          INIT_LIST_HEAD(&atsr->devices);
          list_add_tail(&atsr->node, &s_dmar_atsr_units);

          printk(KERN_DEBUG ACPI_CLASS "ATS Reporting (ATSR): segment=%u, flags=0x%x\n",
                 atsr->segment, atsr->flags);

          parse_device_scope(&atsr->devices, (void *)(atsr_acpi + 1),
                             atsr_acpi->entry.length - sizeof(struct acpi_dmar_atsr_header));
        }
        break;
      }
      default:
        printk(KERN_NOTICE ACPI_CLASS "DMAR: subtable type %u length %u ignored\n",
               sub_hdr->type, sub_hdr->length);
        break;
    }

    cursor += sub_hdr->length;
    bytes_left -= sub_hdr->length;
  }

  return 0;
}

int dmar_init(void) {
  uacpi_table tbl;
  auto status = uacpi_table_find_by_signature(ACPI_SIG_DMAR, &tbl);
  if (uacpi_unlikely_error(status)) {
    return -ENODEV;
  }

  printk(KERN_INFO ACPI_CLASS "Parsing DMAR (DMA Remapping Reporting Table)...\n");
  dmar_parse_subtables((struct acpi_dmar_header *)tbl.hdr);

  uacpi_table_unref(&tbl);
  return 0;
}

struct list_head *dmar_get_units(void) { return &s_dmar_units; }
struct list_head *dmar_get_reserved_regions(void) { return &s_dmar_reserved_regions; }
struct list_head *dmar_get_atsr_units(void) { return &s_dmar_atsr_units; }

EXPORT_SYMBOL(dmar_init);
EXPORT_SYMBOL(dmar_get_units);
EXPORT_SYMBOL(dmar_get_reserved_regions);
EXPORT_SYMBOL(dmar_get_atsr_units);
