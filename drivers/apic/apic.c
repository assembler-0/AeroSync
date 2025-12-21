/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/apic.c
 * @brief APIC abstraction layer and mode selection
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 */

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <drivers/apic/apic.h>
#include <drivers/apic/apic_internal.h>
#include <drivers/apic/ioapic.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <kernel/classes.h>
#include <lib/printk.h>
#include <drivers/apic/pic.h>

// --- Constants ---
#define MAX_IRQ_OVERRIDES 16

// --- Global Variables (Consolidated) ---

// These are accessed by xapic.c (via extern)
uacpi_u64 xapic_madt_lapic_override_phys = 0;   // 0 if not provided
int xapic_madt_parsed = 0;

// IOAPIC physical address (used internally here)
static uacpi_u32 apic_madt_ioapic_phys = 0;

// Interrupt Overrides
struct acpi_madt_interrupt_source_override apic_irq_overrides[MAX_IRQ_OVERRIDES];
int apic_num_irq_overrides = 0;

// Current APIC Ops
static const struct apic_ops *current_ops = NULL;

// --- Forward Declarations ---
static int detect_apic(void);
static void apic_parse_madt(void);

// --- APIC Mode Detection and Selection ---

static int detect_x2apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx & (1 << 21)) != 0; // Check for x2APIC feature bit
}

static int detect_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx & (1 << 9)) != 0; // Check for APIC feature bit
}

// --- MADT parsing via uACPI ---
static uacpi_iteration_decision apic_madt_iter_cb(uacpi_handle user, struct acpi_entry_hdr* ehdr) {
    (void)user;
    switch (ehdr->type) {
        case ACPI_MADT_ENTRY_TYPE_LAPIC_ADDRESS_OVERRIDE: {
            const struct acpi_madt_lapic_address_override* ovr = (const void*)ehdr;
            xapic_madt_lapic_override_phys = ovr->address;
            break;
        }
        case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
            const struct acpi_madt_ioapic* io = (const void*)ehdr;
            if (apic_madt_ioapic_phys == 0) {
                apic_madt_ioapic_phys = io->address;
            }
            break;
        }
        case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
            const struct acpi_madt_interrupt_source_override* iso = (const void*)ehdr;
            if (apic_num_irq_overrides < MAX_IRQ_OVERRIDES) {
                apic_irq_overrides[apic_num_irq_overrides] = *iso;
                apic_num_irq_overrides++;
            }
            break;
        }
        default:
            break;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static void apic_parse_madt(void) {
    if (xapic_madt_parsed) return;

    uacpi_table tbl;
    uacpi_status st = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl);
    if (uacpi_likely_success(st)) {
        uacpi_for_each_subtable(tbl.hdr, sizeof(struct acpi_madt), apic_madt_iter_cb, NULL);
        uacpi_table_unref(&tbl);
        xapic_madt_parsed = 1;
    } else {
        xapic_madt_parsed = 1; // Mark parsed anyway to avoid retry
    }
}

// --- Core APIC Functions (Abstraction Layer) ---

int apic_init(void) {
    pic_mask_all();

    // Parse MADT via uACPI
    apic_parse_madt();

    // Select Driver
    if (detect_x2apic()) {
        printk(APIC_CLASS "x2APIC mode supported, attempting to enable\n");
        // Try x2APIC
        if (x2apic_ops.init_lapic()) {
            current_ops = &x2apic_ops;
            printk(APIC_CLASS "x2APIC mode enabled\n");
        }
    }
    
    if (!current_ops) {
        // Fallback or default to xAPIC
        if (xapic_ops.init_lapic()) {
            current_ops = &xapic_ops;
            printk(APIC_CLASS "xAPIC mode enabled\n");
        }
    }

    if (!current_ops) {
        printk(KERN_ERR APIC_CLASS "Failed to initialize Local APIC (no driver).\n");
        return 0;
    }

    // Initialize I/O APIC
    uint64_t ioapic_phys = apic_madt_ioapic_phys 
                         ? (uint64_t)apic_madt_ioapic_phys 
                         : (uint64_t)IOAPIC_DEFAULT_PHYS_ADDR;
    
    if (!ioapic_init(ioapic_phys)) {
        printk(KERN_ERR APIC_CLASS "Failed to setup I/O APIC.\n");
        return 0;
    }

    return 1;
}

int apic_probe(void) {
    return detect_apic();
}

void apic_send_eoi(const uint32_t irn) { 
    if (current_ops && current_ops->send_eoi)
        current_ops->send_eoi(irn);
}

void apic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    if (current_ops && current_ops->send_ipi) {
        // We pass the 8-bit dest_apic_id. The driver handles format.
        // x2APIC driver expects 32-bit ID, but 8-bit fits in 32-bit.
        current_ops->send_ipi((uint32_t)dest_apic_id, vector, delivery_mode);
    }
}

uint8_t lapic_get_id(void) {
    if (current_ops && current_ops->get_id) {
        return (uint8_t)current_ops->get_id();
    }
    return 0;
}

void apic_enable_irq(uint8_t irq_line) {
    // 1. Get Destination Local APIC ID
    // We route to the current CPU (BSP or whoever enabled it).
    // Typically BSP for legacy IRQs.
    uint32_t dest_apic_id = 0;
    if (current_ops && current_ops->get_id) {
        dest_apic_id = current_ops->get_id();
    }

    // 2. Resolve GSI and Flags from Overrides
    uint32_t gsi = irq_line;
    uint16_t flags = 0;

    for (int i = 0; i < apic_num_irq_overrides; i++) {
        if (apic_irq_overrides[i].source == irq_line) {
            gsi = apic_irq_overrides[i].gsi;
            flags = apic_irq_overrides[i].flags;
            break;
        }
    }

    // 3. Determine if we are in x2APIC mode for IOAPIC format
    // If current_ops is x2apic_ops, we use x2apic format.
    int is_x2apic = (current_ops == &x2apic_ops);

    // 4. Call IOAPIC driver
    // Vector = 32 + irq_line
    ioapic_set_gsi_redirect(gsi, 32 + irq_line, dest_apic_id, flags, 0 /* physical */, is_x2apic);
}

void apic_disable_irq(uint8_t irq_line) {
    // Resolve GSI
    uint32_t gsi = irq_line;
    for (int i = 0; i < apic_num_irq_overrides; i++) {
        if (apic_irq_overrides[i].source == irq_line) {
            gsi = apic_irq_overrides[i].gsi;
            break;
        }
    }
    ioapic_mask_gsi(gsi);
}

void apic_mask_all(void) {
    ioapic_mask_all();
}

void apic_timer_init(uint32_t frequency_hz) {
    if (current_ops && current_ops->timer_init)
        current_ops->timer_init(frequency_hz);
}

void apic_timer_set_frequency(uint32_t frequency_hz) {
    if (current_ops && current_ops->timer_set_frequency)
        current_ops->timer_set_frequency(frequency_hz);
}

static void apic_shutdown(void) {
    apic_mask_all();
    if (current_ops && current_ops->shutdown)
        current_ops->shutdown();
    printk(APIC_CLASS "APIC shut down.\n");
}

static const interrupt_controller_interface_t apic_interface = {
    .type = INTC_APIC,
    .probe = apic_probe,
    .install = apic_init,
    .timer_set = apic_timer_init,
    .enable_irq = apic_enable_irq,
    .disable_irq = apic_disable_irq,
    .send_eoi = apic_send_eoi,
    .mask_all = apic_mask_all,
    .shutdown = apic_shutdown,
    .priority = 100,
};

const interrupt_controller_interface_t* apic_get_driver(void) {
    return &apic_interface;
}