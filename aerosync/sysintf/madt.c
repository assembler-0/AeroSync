///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/madt.c
 * @brief Generic MADT Parser Implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <aerosync/sysintf/madt.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <lib/printk.h>
#include <uacpi/uacpi.h>
#include <uacpi/platform/types.h>

static uint64_t s_lapic_address = 0xfee00000; // Default x86 base
static madt_ioapic_t s_ioapics[MADT_MAX_IOAPICS];
static size_t s_num_ioapics = 0;

static madt_iso_t s_isos[MADT_MAX_ISO];
static size_t s_num_isos = 0;

static madt_lapic_nmi_t s_lapic_nmis[MADT_MAX_LAPIC_NMIS];
static size_t s_num_lapic_nmis = 0;

static uacpi_iteration_decision madt_iter_cb(uacpi_handle user, struct acpi_entry_hdr *ehdr) {
  (void) user;

  switch (ehdr->type) {
    case ACPI_MADT_ENTRY_TYPE_LAPIC: {
      // We could track individual LAPICs here if needed for SMP
      break;
    }
    case ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE: {
      const struct acpi_madt_lapic_address_override *ovr = (const void *) ehdr;
      s_lapic_address = ovr->address;
      break;
    }
    case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
      if (s_num_ioapics < MADT_MAX_IOAPICS) {
        const struct acpi_madt_ioapic *io = (const void *) ehdr;
        s_ioapics[s_num_ioapics].id = io->id;
        s_ioapics[s_num_ioapics].address = io->address;
        s_ioapics[s_num_ioapics].gsi_base = io->gsi_base;
        s_num_ioapics++;
      }
      break;
    }
    case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
      if (s_num_isos < MADT_MAX_ISO) {
        const struct acpi_madt_interrupt_source_override *iso = (const void *) ehdr;
        s_isos[s_num_isos].bus = iso->bus;
        s_isos[s_num_isos].source = iso->source;
        s_isos[s_num_isos].gsi = iso->gsi;
        s_isos[s_num_isos].flags = iso->flags;
        s_num_isos++;
      }
      break;
    }
    case ACPI_MADT_ENTRY_TYPE_LAPIC_NMI: {
      if (s_num_lapic_nmis < MADT_MAX_LAPIC_NMIS) {
        const struct acpi_madt_lapic_nmi *nmi = (const void *) ehdr;
        s_lapic_nmis[s_num_lapic_nmis].processor_id = nmi->uid;
        s_lapic_nmis[s_num_lapic_nmis].flags = nmi->flags;
        s_lapic_nmis[s_num_lapic_nmis].lint = nmi->lint;
        s_num_lapic_nmis++;
      }
      break;
    }
  }

  return UACPI_ITERATION_DECISION_CONTINUE;
}

int madt_init(void) {
  uacpi_table tbl;
  uacpi_status st = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl);
  if (uacpi_unlikely_error(st)) {
    printk(KERN_WARNING ACPI_CLASS "MADT not found, using defaults\n");
    return -1;
  }

  // First, get the standard LAPIC address from the MADT header
  struct acpi_madt *madt_hdr = (struct acpi_madt *) tbl.hdr;
  s_lapic_address = madt_hdr->local_interrupt_controller_address;

  // Iterate subtables for overrides and IOAPICs
  uacpi_for_each_subtable(tbl.hdr, sizeof(struct acpi_madt), madt_iter_cb, NULL);

  uacpi_table_unref(&tbl);

  printk(KERN_INFO ACPI_CLASS "MADT parsed: %zu IOAPICs, %zu ISOs, %zu LAPIC NMIs\n",
         s_num_ioapics, s_num_isos, s_num_lapic_nmis);
  printk(KERN_DEBUG ACPI_CLASS "Local APIC Address: 0x%llx\n", s_lapic_address);

      return 0;
  }
  
  EXPORT_SYMBOL(madt_init);
  
  uint64_t madt_get_lapic_address(void) {  return s_lapic_address;
}

const madt_ioapic_t *madt_get_ioapics(size_t *out_count) {
  *out_count = s_num_ioapics;
  return s_ioapics;
}

const madt_iso_t *madt_get_overrides(size_t *out_count) {
  *out_count = s_num_isos;
  return s_isos;
}

const madt_lapic_nmi_t *madt_get_lapic_nmis(size_t *out_count) {
  *out_count = s_num_lapic_nmis;
  return s_lapic_nmis;
}

EXPORT_SYMBOL(madt_get_lapic_address);
EXPORT_SYMBOL(madt_get_ioapics);
EXPORT_SYMBOL(madt_get_overrides);
