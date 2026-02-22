///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/madt.c
 * @brief Generic MADT Parser Implementation using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <acpi.h>
#include <aerosync/sysintf/madt.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <lib/printk.h>

static uint64_t s_lapic_address = 0xfee00000;
static madt_ioapic_t s_ioapics[MADT_MAX_IOAPICS];
static size_t s_num_ioapics = 0;

static madt_iso_t s_isos[MADT_MAX_ISO];
static size_t s_num_isos = 0;

static madt_lapic_nmi_t s_lapic_nmis[MADT_MAX_LAPIC_NMIS];
static size_t s_num_lapic_nmis = 0;

int madt_init(void) {
  ACPI_TABLE_MADT *madt;
  ACPI_STATUS st = AcpiGetTable(ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER **)&madt);
  if (ACPI_FAILURE(st)) {
    printk(KERN_WARNING ACPI_CLASS "MADT not found, using defaults\n");
    return -ENODEV;
  }

  s_lapic_address = madt->Address;

  ACPI_SUBTABLE_HEADER *sub = (ACPI_SUBTABLE_HEADER *)(madt + 1);
  uint8_t *end = (uint8_t *)madt + madt->Header.Length;

  while ((uint8_t *)sub < end) {
    switch (sub->Type) {
      case ACPI_MADT_TYPE_LOCAL_APIC:
        break;
      case ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE: {
        ACPI_MADT_LOCAL_APIC_OVERRIDE *ovr = (ACPI_MADT_LOCAL_APIC_OVERRIDE *)sub;
        s_lapic_address = ovr->Address;
        break;
      }
      case ACPI_MADT_TYPE_IO_APIC: {
        if (s_num_ioapics < MADT_MAX_IOAPICS) {
          ACPI_MADT_IO_APIC *io = (ACPI_MADT_IO_APIC *)sub;
          s_ioapics[s_num_ioapics].id = io->Id;
          s_ioapics[s_num_ioapics].address = io->Address;
          s_ioapics[s_num_ioapics].gsi_base = io->GlobalIrqBase;
          s_num_ioapics++;
        }
        break;
      }
      case ACPI_MADT_TYPE_INTERRUPT_OVERRIDE: {
        if (s_num_isos < MADT_MAX_ISO) {
          ACPI_MADT_INTERRUPT_OVERRIDE *iso = (ACPI_MADT_INTERRUPT_OVERRIDE *)sub;
          s_isos[s_num_isos].bus = iso->Bus;
          s_isos[s_num_isos].source = iso->SourceIrq;
          s_isos[s_num_isos].gsi = iso->GlobalIrq;
          s_isos[s_num_isos].flags = iso->IntiFlags;
          s_num_isos++;
        }
        break;
      }
      case ACPI_MADT_TYPE_LOCAL_APIC_NMI: {
        if (s_num_lapic_nmis < MADT_MAX_LAPIC_NMIS) {
          ACPI_MADT_LOCAL_APIC_NMI *nmi = (ACPI_MADT_LOCAL_APIC_NMI *)sub;
          s_lapic_nmis[s_num_lapic_nmis].processor_id = nmi->ProcessorId;
          s_lapic_nmis[s_num_lapic_nmis].flags = nmi->IntiFlags;
          s_lapic_nmis[s_num_lapic_nmis].lint = nmi->Lint;
          s_num_lapic_nmis++;
        }
        break;
      }
    }
    sub = (ACPI_SUBTABLE_HEADER *)((uint8_t *)sub + sub->Length);
  }

  printk(KERN_INFO ACPI_CLASS "MADT parsed: %zu IOAPICs, %zu ISOs, %zu LAPIC NMIs\n",
         s_num_ioapics, s_num_isos, s_num_lapic_nmis);
  printk(KERN_DEBUG ACPI_CLASS "Local APIC Address: 0x%llx\n", s_lapic_address);

  return 0;
}

#include <aerosync/export.h>
EXPORT_SYMBOL(madt_init);

uint64_t madt_get_lapic_address(void) {
  return s_lapic_address;
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
