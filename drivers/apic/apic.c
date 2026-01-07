/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/apic/apic.c
 * @brief APIC abstraction system
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

#include <arch/x86_64/io.h>
#include <drivers/apic/apic_internal.h>
#include <drivers/apic/ioapic.h>
#include <drivers/apic/pic.h>
#include <kernel/classes.h>
#include <kernel/fkx/fkx.h>
#include <uacpi/platform/types.h>

// --- Register Definitions for Calibration ---
// xAPIC MMIO Offsets
#define XAPIC_LVT_TIMER_REG 0x0320
#define XAPIC_TIMER_INIT_COUNT_REG 0x0380
#define XAPIC_TIMER_CUR_COUNT_REG 0x0390
#define XAPIC_TIMER_DIV_REG 0x03E0

// x2APIC MSRs
#define X2APIC_LVT_TIMER_MSR 0x832
#define X2APIC_TIMER_INIT_CNT_MSR 0x838
#define X2APIC_TIMER_CUR_CNT_MSR 0x839
#define X2APIC_TIMER_DIV_MSR 0x83E

struct apic_timer_regs {
    uint32_t lvt_timer;
    uint32_t init_count;
    uint32_t cur_count;
    uint32_t div;
};

// --- Constants ---
#define MAX_IRQ_OVERRIDES 16

// --- Global Variables (Consolidated) ---
uacpi_u64 xapic_madt_lapic_override_phys = 0;   // 0 if not provided
int xapic_madt_parsed = 0;
static uacpi_u32 apic_madt_ioapic_phys = 0;
struct acpi_madt_interrupt_source_override apic_irq_overrides[MAX_IRQ_OVERRIDES];
int apic_num_irq_overrides = 0;
static const struct apic_ops *current_ops = NULL;
static uint32_t apic_calibrated_ticks = 0;


// --- Forward Declarations ---
static int detect_apic(void);
static void apic_parse_madt(void);
static void apic_timer_calibrate(const struct apic_timer_regs *regs);
static void apic_shutdown(void);
int apic_init_ap(void);


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
    apic_parse_madt();

    if (detect_x2apic()) {
        printk(KERN_DEBUG APIC_CLASS "x2APIC mode supported, attempting to enable\n");
        if (x2apic_ops.init_lapic()) {
            current_ops = &x2apic_ops;
            printk(KERN_DEBUG APIC_CLASS "x2APIC mode enabled\n");
        }
    }
    
    if (!current_ops) {
        if (xapic_ops.init_lapic()) {
            current_ops = &xapic_ops;
            printk(KERN_DEBUG APIC_CLASS "xAPIC mode enabled\n");
        }
    }

    if (!current_ops) {
        printk(KERN_ERR APIC_CLASS "Failed to initialize Local APIC (no driver).\n");
        return 0;
    }

    // Calibrate timer with mode-specific registers
    if (current_ops == &x2apic_ops) {
        const struct apic_timer_regs regs = {
            .lvt_timer = X2APIC_LVT_TIMER_MSR,
            .init_count = X2APIC_TIMER_INIT_CNT_MSR,
            .cur_count = X2APIC_TIMER_CUR_CNT_MSR,
            .div = X2APIC_TIMER_DIV_MSR
        };
        apic_timer_calibrate(&regs);
    } else {
        const struct apic_timer_regs regs = {
            .lvt_timer = XAPIC_LVT_TIMER_REG,
            .init_count = XAPIC_TIMER_INIT_COUNT_REG,
            .cur_count = XAPIC_TIMER_CUR_COUNT_REG,
            .div = XAPIC_TIMER_DIV_REG
        };
        apic_timer_calibrate(&regs);
    }
    
    uint64_t ioapic_phys = apic_madt_ioapic_phys 
                         ? (uint64_t)apic_madt_ioapic_phys 
                         : (uint64_t)IOAPIC_DEFAULT_PHYS_ADDR;
    
    if (!ioapic_init(ioapic_phys)) {
        printk(KERN_ERR APIC_CLASS "Failed to setup I/O APIC.\n");
        return 0;
    }

    return 1;
}

int apic_init_ap(void) {
    if (!current_ops || !current_ops->init_lapic) {
        printk(KERN_ERR APIC_CLASS "APIC driver not initialized on BSP.\n");
        return 0;
    }
    return current_ops->init_lapic();
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
    uint32_t dest_apic_id = 0;
    if (current_ops && current_ops->get_id) {
        dest_apic_id = current_ops->get_id();
    }

    uint32_t gsi = irq_line;
    uint16_t flags = 0;

    for (int i = 0; i < apic_num_irq_overrides; i++) {
        if (apic_irq_overrides[i].source == irq_line) {
            gsi = apic_irq_overrides[i].gsi;
            flags = apic_irq_overrides[i].flags;
            break;
        }
    }

    int is_x2apic = (current_ops == &x2apic_ops);
    ioapic_set_gsi_redirect(gsi, 32 + irq_line, dest_apic_id, flags, 0, is_x2apic);
}

void apic_disable_irq(uint8_t irq_line) {
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

static void apic_timer_calibrate(const struct apic_timer_regs *regs) {
    if (!current_ops || !current_ops->write || !current_ops->read) return;

    current_ops->write(regs->div, 0x3);
    current_ops->write(regs->lvt_timer, (1 << 16));

    // Bochs may have timing issues with the standard calibration
    // Use a more robust approach that includes verification
    uint16_t pit_reload = 11931; // ~10ms at 1193180 Hz
    outb(0x61, (inb(0x61) & 0xFD) | 1);
    outb(0x43, 0xB0);
    outb(0x42, pit_reload & 0xFF);
    outb(0x42, (pit_reload >> 8) & 0xFF);

    current_ops->write(regs->init_count, 0xFFFFFFFF);

    // Wait for PIT countdown to complete (with timeout to avoid infinite loops in emulators)
    uint32_t timeout = 0x1000000; // Reasonable timeout
    while (!(inb(0x61) & 0x20) && timeout--);

    if (timeout == 0) {
        // If timeout occurred, use a reasonable default for the timer calibration
        // This can happen in some emulators like Bochs with timing issues
        apic_calibrated_ticks = 100000; // Reasonable default for 10ms
        printk(KERN_NOTICE APIC_CLASS "Timer calibration timeout, using default: %u ticks in 10ms.\n", apic_calibrated_ticks);
    } else {
        current_ops->write(regs->lvt_timer, (1 << 16));
        apic_calibrated_ticks = 0xFFFFFFFF - current_ops->read(regs->cur_count);
        printk(KERN_DEBUG APIC_CLASS "Calibrated timer: %u ticks in 10ms.\n", apic_calibrated_ticks);
    }

    // Additional safety check: if calibration result is unreasonable, use default
    if (apic_calibrated_ticks < 1000 || apic_calibrated_ticks > 10000000) {
        printk(KERN_NOTICE APIC_CLASS "Calibration result unreasonable (%u), using default.\n", apic_calibrated_ticks);
        apic_calibrated_ticks = 100000; // Reasonable default
    }
}

void apic_timer_set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0) return;

    uint32_t ticks_per_target;

    if (apic_calibrated_ticks == 0) {
        // If calibration failed, use a reasonable default based on common frequencies
        // For 100Hz timer with typical APIC clock, use a default value
        ticks_per_target = 1000000 / frequency_hz; // 1MHz default estimate
        printk(KERN_NOTICE APIC_CLASS "Using default timer value for %u Hz: %u ticks\n",
               frequency_hz, ticks_per_target);
    } else {
        ticks_per_target = (apic_calibrated_ticks * 100) / frequency_hz;
    }

    // Ensure the ticks value is reasonable (not too small or too large)
    if (ticks_per_target < 100) {
        ticks_per_target = 100;
        printk(KERN_NOTICE APIC_CLASS "Adjusted timer ticks to minimum safe value: %u\n", ticks_per_target);
    } else if (ticks_per_target > 0x7FFFFFFF) { // Avoid potential overflow issues
        ticks_per_target = 0x7FFFFFFF;
        printk(KERN_NOTICE APIC_CLASS "Adjusted timer ticks to maximum safe value: %u\n", ticks_per_target);
    }

    if (current_ops && current_ops->timer_set_frequency) {
        current_ops->timer_set_frequency(ticks_per_target);
    }
}

static void apic_shutdown(void) {
    apic_mask_all();
    if (current_ops && current_ops->shutdown)
        current_ops->shutdown();
    printk(KERN_DEBUG APIC_CLASS "APIC shut down.\n");
}

static const interrupt_controller_interface_t apic_interface = {
    .type = INTC_APIC,
    .probe = apic_probe,
    .install = apic_init,
    .init_ap = apic_init_ap,
    .timer_set = apic_timer_set_frequency,
    .enable_irq = apic_enable_irq,
    .disable_irq = apic_disable_irq,
    .send_eoi = apic_send_eoi,
    .mask_all = apic_mask_all,
    .shutdown = apic_shutdown,
    .priority = 100,
    .send_ipi = apic_send_ipi,
    .get_id = lapic_get_id,
};

const interrupt_controller_interface_t* apic_get_driver(void) {
    return &apic_interface;
}
