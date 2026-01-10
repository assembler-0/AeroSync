///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/apic/ioapic.c
 * @brief I/O APIC driver implementation
 * @copyright (C) 2025 assembler-0
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

#include <arch/x86_64/mm/paging.h>
#include <drivers/apic/ioapic.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <uacpi/acpi.h>
#include <mm/vmalloc.h>

static volatile uint32_t *ioapic_base = NULL;

static void ioapic_write(uint8_t reg, uint32_t value) {
    if (!ioapic_base)
        return;
    // I/O APIC uses an index/data pair for access
    ioapic_base[0] = reg;
    ioapic_base[4] = value;
}

static uint32_t ioapic_read(uint8_t reg) {
    if (!ioapic_base)
        return 0;
    ioapic_base[0] = reg;
    return ioapic_base[4];
}

int ioapic_init(uint64_t phys_addr) {
    // Map the I/O APIC into virtual memory.
    ioapic_base = (volatile uint32_t *)viomap(phys_addr, PAGE_SIZE);

    if (!ioapic_base) {
        printk(KERN_ERR APIC_CLASS "Failed to map I/O APIC MMIO.\n");
        return 0;
    }

    printk(KERN_DEBUG APIC_CLASS "IOAPIC Mapped at: 0x%llx (Phys: 0x%llx)\n", (uint64_t)ioapic_base, phys_addr);

    // Read the I/O APIC version to verify it's working
    uint32_t version_reg = ioapic_read(IOAPIC_REG_VER);
    printk(KERN_DEBUG APIC_CLASS "IOAPIC Version: 0x%x\n", version_reg);

    return 1;
}

void ioapic_write_entry(uint8_t index, uint64_t data) {
    ioapic_write(IOAPIC_REG_TABLE + index * 2, (uint32_t)data);
    ioapic_write(IOAPIC_REG_TABLE + index * 2 + 1, (uint32_t)(data >> 32));
}

uint64_t ioapic_read_entry(uint8_t index) {
    uint64_t data = ioapic_read(IOAPIC_REG_TABLE + index * 2);
    data |= ((uint64_t)ioapic_read(IOAPIC_REG_TABLE + index * 2 + 1)) << 32;
    return data;
}

void ioapic_mask_gsi(uint32_t gsi) {
    // To disable, we set the mask bit (bit 16)
    // We overwrite with just the mask bit set (clearing everything else) for safety/simplicity
    // consistent with original driver.
    ioapic_write_entry(gsi, (1 << 16));
}

void ioapic_set_gsi_redirect(uint32_t gsi, uint8_t vector, uint32_t dest_apic_id, uint16_t flags, int dest_mode_logical, int is_x2apic) {
    uint64_t redirect_entry = vector;
    
    // Delivery Mode: Fixed (000)
    redirect_entry |= (0b000ull << 8); 

    // Destination Mode
    if (dest_mode_logical) {
        redirect_entry |= (1ull << 11);
    } else {
        redirect_entry |= (0ull << 11);
    }

    // Handle flags
    uint16_t polarity = flags & ACPI_MADT_POLARITY_MASK;
    uint16_t trigger = flags & ACPI_MADT_TRIGGERING_MASK;

    if (polarity == ACPI_MADT_POLARITY_ACTIVE_LOW) {
        redirect_entry |= (1ull << 13); // Polarity: Low
    } else {
        redirect_entry |= (0ull << 13); // Polarity: High
    }

    if (trigger == ACPI_MADT_TRIGGERING_LEVEL) {
        redirect_entry |= (1ull << 15); // Trigger: Level
    } else {
        redirect_entry |= (0ull << 15); // Trigger: Edge
    }

    // Unmask (bit 16 = 0) - implicit since we don't set it.

    // Destination field
    if (is_x2apic) {
        // x2APIC format: destination in bits 32-63
        redirect_entry |= ((uint64_t)dest_apic_id << 32);
    } else {
        // xAPIC format: destination in bits 56-63
        redirect_entry |= ((uint64_t)(dest_apic_id & 0xFF) << 56);
    }

    ioapic_write_entry(gsi, redirect_entry);
}

void ioapic_mask_all(void) {
    // Mask all redirection entries in the I/O APIC
    // Determine max entries from version register
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    int max_entries = ((ver >> 16) & 0xFF) + 1;
    
    for (int i = 0; i < max_entries; i++) {
        ioapic_mask_gsi(i);
    }
}
