/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/acpi.h
 * @brief Advanced ACPI Table Parsing using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <acpi.h>
#include <aerosync/compiler.h>

/**
 * @brief Initialize all ACPI table managers.
 * Should be called after acpica_kernel_init_early.
 */
int __must_check acpi_tables_init(void);

/* --- FADT (Fixed ACPI Description Table) --- */
typedef ACPI_TABLE_FADT acpi_fadt_t;
struct acpi_fadt *acpi_get_fadt(void);
bool acpi_fadt_supports_reset_reg(void);

/* --- SRAT/SLIT (NUMA) --- */
int acpi_numa_get_node_count(void);
uint8_t acpi_numa_get_distance(int node_from, int node_to);

/* --- WAET (Windows ACPI Emulation Table) --- */
#define ACPI_WAET_SIGNATURE "WAET"
struct acpi_waet {
  ACPI_TABLE_HEADER Header;
  UINT32 flags;
} __packed;

#define ACPI_WAET_RTC_GOOD (1 << 0)
#define ACPI_WAET_PM_TIMER_GOOD (1 << 1)

bool acpi_waet_is_rtc_good(void);
bool acpi_waet_is_pm_timer_good(void);

/* --- MCFG (PCI Express ECAM) --- */
#define ACPI_MCFG_SIGNATURE "MCFG"
typedef ACPI_MCFG_ALLOCATION acpi_mcfg_allocation_t;

const struct acpi_mcfg_allocation *acpi_get_mcfg_entries(size_t *out_count);

/* --- SPCR (Serial Port Console Redirection) --- */
#define ACPI_SPCR_SIGNATURE "SPCR"
const struct acpi_spcr *acpi_get_spcr(void);

/* --- BGRT (Boot Graphics Resource Table) --- */
#define ACPI_BGRT_SIGNATURE "BGRT"
const struct acpi_bgrt *acpi_get_bgrt(void);

/* --- HPET (High Precision Event Timer Description Table) --- */
#define ACPI_HPET_SIGNATURE "HPET"
const struct acpi_hpet *acpi_get_hpet(void);

/* --- DSDT (Differentiated System Description Table) --- */
// Accessible via AcpiGetTable(ACPI_SIG_DSDT, ...)

/* --- Device Enumeration --- */
int __must_check acpi_bus_enumerate(void);

/* --- Generic Helper --- */
ACPI_STATUS acpica_find_table(const char *signature, ACPI_TABLE_HEADER **out_table);
