/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/madt.h
 * @brief Generic MADT (Multiple APIC Description Table) Parser Interface
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <uacpi/acpi.h>

#define MADT_MAX_IOAPICS 8
#define MADT_MAX_ISO 16
#define MADT_MAX_LAPIC_NMIS 4

typedef struct {
    uint8_t id;
    uint32_t address;
    uint32_t gsi_base;
} madt_ioapic_t;

typedef struct {
    uint8_t bus;
    uint8_t source;
    uint32_t gsi;
    uint16_t flags;
} madt_iso_t;

typedef struct {
    uint8_t processor_id;
    uint16_t flags;
    uint8_t lint;
} madt_lapic_nmi_t;

/**
 * @brief Initializes the MADT manager by parsing the ACPI MADT table.
 * @return 0 on success, negative on error.
 */
int madt_init(void);

/**
 * @brief Returns the physical address of the Local APIC.
 * Handles the address override if present.
 */
uint64_t madt_get_lapic_address(void);

/**
 * @brief Returns the list of detected I/O APICs.
 * @param out_count Pointer to receive the number of I/O APICs.
 * @return Array of madt_ioapic_t structures.
 */
const madt_ioapic_t* madt_get_ioapics(size_t *out_count);

/**
 * @brief Returns the list of Interrupt Source Overrides.
 * @param out_count Pointer to receive the number of overrides.
 * @return Array of madt_iso_t structures.
 */
const madt_iso_t* madt_get_overrides(size_t *out_count);

/**
 * @brief Returns the list of Local APIC NMIs.
 */
const madt_lapic_nmi_t* madt_get_lapic_nmis(size_t *out_count);
