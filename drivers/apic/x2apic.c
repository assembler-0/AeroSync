/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/x2apic.c
 * @brief x2APIC driver
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <arch/x64/io.h>
#include <arch/x64/mm/pmm.h>
#include <kernel/classes.h>
#include <lib/printk.h>
#include <mm/vmalloc.h>
#include <kernel/spinlock.h>
#include <drivers/apic/x2apic.h>

// --- x2APIC MSR Addresses ---

#define X2APIC_ID             0x00000802ULL  // Local APIC ID
#define X2APIC_VERSION        0x00000803ULL  // Local APIC Version
#define X2APIC_TPR            0x00000808ULL  // Task Priority
#define X2APIC_EOI            0x0000080BULL  // EOI
#define X2APIC_LDR            0x0000080DULL  // Logical Destination
#define X2APIC_SVR            0x0000080FUL  // Spurious Interrupt Vector
#define X2APIC_ISR_BASE       0x00000810ULL  // In-Service Register (8 registers)
#define X2APIC_TMR_BASE       0x00000818ULL  // Trigger Mode Register (8 registers)
#define X2APIC_IRR_BASE       0x00000820ULL  // Interrupt Request Register (8 registers)
#define X2APIC_ESR            0x00000828ULL  // Error Status
#define X2APIC_ICR            0x00000830ULL  // Interrupt Command Register (64-bit)
#define X2APIC_LVT_TIMER      0x00000832ULL  // LVT Timer
#define X2APIC_LVT_THERMAL    0x00000833ULL  // LVT Thermal Sensor
#define X2APIC_LVT_PERF       0x00000834ULL  // LVT Performance Counter
#define X2APIC_LVT_LINT0      0x00000835ULL  // LVT LINT0
#define X2APIC_LVT_LINT1      0x00000836ULL  // LVT LINT1
#define X2APIC_LVT_ERROR      0x00000837ULL  // LVT Error
#define X2APIC_TIMER_INIT_CNT 0x00000838ULL  // Timer Initial Count
#define X2APIC_TIMER_CUR_CNT  0x00000839ULL  // Timer Current Count
#define X2APIC_TIMER_DIV      0x0000083EULL  // Timer Divide Configuration
#define X2APIC_SELF_IPI       0x0000083FUL  // Self IPI (for x2APIC only)

// --- Constants ---
#define APIC_BASE_MSR 0x1B
#define APIC_BASE_MSR_ENABLE 0x800
#define APIC_BASE_MSR_X2APIC_ENABLE (1ULL << 10)

#define IOAPIC_DEFAULT_PHYS_ADDR 0xFEC00000

// --- x2APIC MSR Access Functions ---

static void x2apic_write(uint32_t msr, uint64_t value) {
    wrmsr(msr, value);
}

static uint64_t x2apic_read(uint32_t msr) {
    return rdmsr(msr);
}

uint32_t x2apic_get_id(void) {
    // In x2APIC mode, the full 32-bit ID is available directly
    return (uint32_t)x2apic_read(X2APIC_ID);
}

void x2apic_send_eoi(const uint32_t irn) { // arg for compatibility
    (void)irn;
    wrmsr(X2APIC_EOI, 0);
}

// Sends an Inter-Processor Interrupt (IPI) using x2APIC MSR
static spinlock_t x2apic_ipi_lock;

void x2apic_send_ipi(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    irq_flags_t flags = spinlock_lock_irqsave(&x2apic_ipi_lock);

    // In x2APIC mode, the ICR is a 64-bit MSR
    // Bits 0-7: Vector
    // Bits 8-10: Delivery Mode
    // Bits 11: Destination Mode (0=physical, 1=logical)
    // Bits 12-14: Reserved
    // Bits 15: Level (1=assert, 0=deassert)
    // Bits 16: Trigger Mode (0=edge, 1=level)
    // Bits 17-19: Reserved
    // Bits 20-31: Destination (APIC ID in x2APIC mode)
    // Bits 32-63: Reserved
    
    uint64_t icr_value = (uint64_t)vector | 
                         (uint64_t)delivery_mode | 
                         (1ULL << 14) /* Reserved */ | 
                         (1ULL << 15) /* Assert Level */ | 
                         (0ULL << 16) /* Edge Trigger */ |
                         ((uint64_t)dest_apic_id << 32);

    // Wait for previous IPI to be delivered
    uint32_t timeout = 100000;
    while (x2apic_read(X2APIC_ICR) & (1ULL << 12)) {
        if (--timeout == 0) {
            printk(KERN_ERR "ICR stuck busy before send (dest: %u)\n", dest_apic_id);
            spinlock_unlock_irqrestore(&x2apic_ipi_lock, flags);
            return;
        }
        cpu_relax();
    }

    x2apic_write(X2APIC_ICR, icr_value);

    // Wait for delivery to complete
    timeout = 100000;
    while (x2apic_read(X2APIC_ICR) & (1ULL << 12)) {
        if (--timeout == 0) {
            printk(KERN_ERR "IPI delivery timeout to APIC ID %u\n", dest_apic_id);
            break;
        }
        cpu_relax();
    }

    spinlock_unlock_irqrestore(&x2apic_ipi_lock, flags);
}

// Initialize x2APIC by enabling x2APIC mode in the APIC_BASE MSR
int x2apic_setup_lapic(void) {
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    
    // Check if x2APIC is supported by checking CPUID
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 21))) { // Check for x2APIC feature bit (ECX bit 21)
        printk(KERN_ERR APIC_CLASS  "x2APIC feature not supported by CPU\n");
        return false;
    }
    
    printk(APIC_CLASS "Enabling x2APIC mode\n");

    // Enable x2APIC mode: set both APIC Global Enable and x2APIC Enable bits
    wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE);

    // Verify x2APIC is enabled by reading the version register
    uint64_t version = x2apic_read(X2APIC_VERSION);
    printk(APIC_CLASS "Version: 0x%llx\n", version & 0xFF);

    // Set Spurious Interrupt Vector (0xFF) and enable APIC (bit 8)
    x2apic_write(X2APIC_SVR, 0x1FF);

    // Set TPR to 0 to accept all interrupts
    x2apic_write(X2APIC_TPR, 0);
    
    return true;
}

// x2APIC does not change I/O APIC handling, so we can reuse xAPIC I/O APIC functions
// But we'll define the same interface for consistency
int x2apic_setup_ioapic(void) {
    // Map the I/O APIC into virtual memory. Prefer MADT-provided address.
    uacpi_u64 ioapic_phys = x2apic_madt_parsed && x2apic_madt_ioapic_phys
                                ? (uacpi_u64)x2apic_madt_ioapic_phys
                                : (uacpi_u64)IOAPIC_DEFAULT_PHYS_ADDR;

    if (ioapic_phys != IOAPIC_DEFAULT_PHYS_ADDR)
        printk(APIC_CLASS "IOAPIC Physical Base from MADT: 0x%llx\n", ioapic_phys);

    x2apic_ioapic_base = (volatile uint32_t *)viomap(ioapic_phys, PAGE_SIZE);

    if (!x2apic_ioapic_base) {
        printk(KERN_ERR APIC_CLASS "Failed to map I/O APIC MMIO.\n");
        return false;
    }

    printk(APIC_CLASS "IOAPIC Mapped at: 0x%llx\n", (uint64_t)x2apic_ioapic_base);

    // Read the I/O APIC version to verify it's working (still uses MMIO)
    uint32_t id_reg = x2apic_ioapic_base[0] = 0x00;  // ID register
    uint32_t version_reg = x2apic_ioapic_base[4];     // Read value
    printk(APIC_CLASS "IOAPIC Version: 0x%x\n", version_reg & 0xFF);

    return true;
}

// I/O APIC functions still use MMIO (x2APIC only affects Local APIC access)
static void ioapic_write(uint8_t reg, uint32_t value) {
    if (!x2apic_ioapic_base)
        return;
    // I/O APIC uses an index/data pair for access
    x2apic_ioapic_base[0] = reg;
    x2apic_ioapic_base[4] = value;
}

static uint32_t ioapic_read(uint8_t reg) {
    if (!x2apic_ioapic_base)
        return 0;
    x2apic_ioapic_base[0] = reg;
    return x2apic_ioapic_base[4];
}

// Sets a redirection table entry in the I/O APIC
static void ioapic_set_entry(uint8_t index, uint64_t data) {
    ioapic_write(0x10 + index * 2, (uint32_t)data);      // IOAPIC_REG_TABLE + index * 2
    ioapic_write(0x10 + index * 2 + 1, (uint32_t)(data >> 32)); // IOAPIC_REG_TABLE + index * 2 + 1
}

void x2apic_enable_irq(uint8_t irq_line) {
    // IRQ line -> Vector 32 + IRQ
    // Route to the current CPU's LAPIC ID (BSP)
    uint32_t dest_apic_id = x2apic_get_id();  // x2APIC ID is 32-bit

    uint32_t gsi = irq_line;
    uint16_t flags = 0;

    for (int i = 0; i < x2apic_num_irq_overrides; i++) {
        if (x2apic_irq_overrides[i].source == irq_line) {
            gsi = x2apic_irq_overrides[i].gsi;
            flags = x2apic_irq_overrides[i].flags;
            break;
        }
    }

    uint64_t redirect_entry = (32 + irq_line); // Vector
    redirect_entry |= (0b000ull << 8);         // Delivery Mode: Fixed
    redirect_entry |= (0b0ull << 11);          // Destination Mode: Physical

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

    // Unmask (bit 16 = 0)
    // Destination field (bits 56..63) - for x2APIC, destination is 32-bit in bits 32-63
    redirect_entry |= ((uint64_t)dest_apic_id << 32);

    ioapic_set_entry(gsi, redirect_entry);
}

void x2apic_disable_irq(uint8_t irq_line) {
    uint32_t gsi = irq_line;

    for (int i = 0; i < x2apic_num_irq_overrides; i++) {
        if (x2apic_irq_overrides[i].source == irq_line) {
            gsi = x2apic_irq_overrides[i].gsi;
            break;
        }
    }

    // To disable, we set the mask bit (bit 16)
    uint64_t redirect_entry = (1 << 16);
    ioapic_set_entry(gsi, redirect_entry);
}

void x2apic_mask_all(void) {
    // Mask all 24 redirection entries in the I/O APIC
    for (int i = 0; i < 24; i++) {
        x2apic_disable_irq(i);
    }
}

void x2apic_timer_init(uint32_t frequency_hz) {
    // Calibrate and set the initial count
    x2apic_timer_set_frequency(frequency_hz);
    printk(APIC_CLASS "Timer installed at %d Hz.\n", frequency_hz);
}

void x2apic_timer_set_frequency(uint32_t frequency_hz) {
    if (frequency_hz == 0)
        return;

    x2apic_timer_hz = frequency_hz;

    // Use PIT to calibrate
    // 1. Prepare LAPIC Timer: Divide by 16, One-shot, Masked
    x2apic_write(X2APIC_TIMER_DIV, 0x3);       // Divide by 16
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16)); // Masked

    // 2. Prepare PIT: Channel 2, Mode 0, Rate = 11931 (approx 10ms)
    // 1193182 Hz / 11931 ~= 100 Hz (10ms)
    uint16_t pit_reload = 11931;
    outb(0x61, (inb(0x61) & 0xFD) | 1); // Gate high
    outb(0x43, 0xB0);                   // Channel 2, Access Lo/Hi, Mode 0, Binary
    outb(0x42, pit_reload & 0xFF);
    outb(0x42, (pit_reload >> 8) & 0xFF);

    // 3. Reset APIC Timer to -1
    x2apic_write(X2APIC_TIMER_INIT_CNT, 0xFFFFFFFF);

    // 4. Wait for PIT to wrap (10ms)
    while (!(inb(0x61) & 0x20))
        ;

    // 5. Stop APIC timer
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16));

    // 6. Calculate ticks
    uint64_t ticks_in_10ms = 0xFFFFFFFF - x2apic_read(X2APIC_TIMER_CUR_CNT);

    // 7. Calculate ticks per period for target frequency
    // ticks_in_10ms corresponds to 100Hz (0.01s)
    // ticks_per_sec = ticks_in_10ms * 100
    // target = ticks_per_sec / frequency_hz
    uint32_t ticks_per_target = (ticks_in_10ms * 100) / frequency_hz;

    // 8. Start Timer: Periodic, Interrupt Vector 32, Unmasked
    // Bit 17 = 1 for Periodic mode, Bit 16 = 0 for Unmasked
    uint32_t lvt_timer = 32 | (1 << 17); // Vector 32, Periodic mode, Unmasked
    x2apic_write(X2APIC_LVT_TIMER, lvt_timer);
    x2apic_write(X2APIC_TIMER_DIV, 0x3); // Divide by 16
    x2apic_write(X2APIC_TIMER_INIT_CNT, ticks_per_target);

    printk(APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u\n", lvt_timer,
           ticks_per_target);
}

void x2apic_shutdown(void) {
    // 1. Mask all I/O APIC interrupts
    x2apic_mask_all();

    // 2. Disable Local APIC Timer
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16)); // Masked

    // 3. Disable Local APIC via SVR (clear bit 8)
    uint64_t svr = x2apic_read(X2APIC_SVR);
    x2apic_write(X2APIC_SVR, svr & ~(1ULL << 8));

    // 4. Disable x2APIC mode via MSR
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    wrmsr(APIC_BASE_MSR, lapic_base_msr & ~(APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE));
}