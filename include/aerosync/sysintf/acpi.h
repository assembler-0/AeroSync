/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/acpi.h
 * @brief Advanced ACPI Table Parsing using uACPI
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>

/**
 * @brief Initialize all ACPI table managers.
 * Should be called after uacpi_kernel_init_early.
 */
int acpi_tables_init(void);

/* --- FADT (Fixed ACPI Description Table) --- */
struct acpi_fadt *acpi_get_fadt(void);
bool acpi_fadt_supports_reset_reg(void);

/* --- SRAT/SLIT (NUMA) --- */
// These are currently bridged to mm/numa.c, but we can expose more if needed.
int acpi_numa_get_node_count(void);
uint8_t acpi_numa_get_distance(int node_from, int node_to);

/* --- WAET (Windows ACPI Emulation Table) --- */
#define ACPI_WAET_SIGNATURE "WAET"
UACPI_PACKED(struct acpi_waet {
  struct acpi_sdt_hdr hdr;
  uacpi_u32 flags;
})

#define ACPI_WAET_RTC_GOOD (1 << 0)
#define ACPI_WAET_PM_TIMER_GOOD (1 << 1)

bool acpi_waet_is_rtc_good(void);
bool acpi_waet_is_pm_timer_good(void);

/* --- MCFG (PCI Express ECAM) --- */
#define ACPI_MCFG_SIGNATURE "MCFG"
// Using struct acpi_mcfg and struct acpi_mcfg_allocation from uacpi/acpi.h

const struct acpi_mcfg_allocation *acpi_get_mcfg_entries(size_t *out_count);

/* --- SPCR (Serial Port Console Redirection) --- */
#define ACPI_SPCR_SIGNATURE "SPCR"
const struct acpi_spcr *acpi_get_spcr(void);

/* --- BGRT (Boot Graphics Resource Table) --- */
#define ACPI_BGRT_SIGNATURE "BGRT"
UACPI_PACKED(struct acpi_bgrt {
  struct acpi_sdt_hdr hdr;
  uacpi_u16 version;
  uacpi_u8 status;
  uacpi_u8 image_type;
  uacpi_u64 image_address;
  uacpi_u32 image_offset_x;
  uacpi_u32 image_offset_y;
})
const struct acpi_bgrt *acpi_get_bgrt(void);

/* --- HPET (High Precision Event Timer Description Table) --- */
#define ACPI_HPET_SIGNATURE "HPET"
// Using struct acpi_hpet from uacpi/acpi.h

const struct acpi_hpet *acpi_get_hpet(void);

/* --- DSDT (Differentiated System Description Table) --- */
// Accessible via uacpi_get_dsdt() or simply through the namespace.
uacpi_status acpi_load_extras(void);

/* --- Device Enumeration --- */
int acpi_bus_enumerate(void);

/* --- Generic Helper --- */
uacpi_status acpi_find_table(const char *signature, uacpi_table *out_table);
void acpi_unref_table(uacpi_table *tbl);
