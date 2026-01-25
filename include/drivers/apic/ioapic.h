#pragma once

#include <aerosync/types.h>

// I/O APIC registers
#define IOAPIC_REG_ID       0x00
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_TABLE    0x10

#define IOAPIC_DEFAULT_PHYS_ADDR 0xFEC00000

// Initialize the I/O APIC driver.
// phys_addr: Physical address of the I/O APIC (from MADT or default).
// returns: 1 on success, 0 on failure.
int ioapic_init(uint64_t phys_addr);

// Write a raw 64-bit redirection entry.
void ioapic_write_entry(uint8_t index, uint64_t data);

// Read a raw 64-bit redirection entry.
uint64_t ioapic_read_entry(uint8_t index);

// Mask a specific GSI.
void ioapic_mask_gsi(uint32_t gsi);

// Configure a redirection entry.
// gsi: Global System Interrupt number.
// vector: IDT vector.
// dest_apic_id: Destination APIC ID.
// flags: ACPI MADT flags (polarity/trigger).
// dest_mode_logical: 1 for logical destination, 0 for physical.
// is_x2apic: 1 if using x2APIC destination format (32-bit dest at bit 32), 0 for xAPIC (8-bit dest at bit 56).
void ioapic_set_gsi_redirect(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, uint16_t flags, int dest_mode_logical, int is_x2apic);

// Mask all redirection entries.
void ioapic_mask_all(void);
