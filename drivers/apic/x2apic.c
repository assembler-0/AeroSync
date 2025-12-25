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

#include <kernel/classes.h>
#include <drivers/apic/x2apic.h> 
#include <drivers/apic/apic_internal.h> 
#include <kernel/fkx/fkx.h>

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

extern struct fkx_kernel_api *ic_kapi; 

// --- Globals ---
static spinlock_t x2apic_ipi_lock;

// --- x2APIC MSR Access Functions ---

static void x2apic_write(uint32_t msr, uint64_t value) {
    ic_kapi->wrmsr(msr, value);
}

static uint64_t x2apic_read(uint32_t msr) {
    return ic_kapi->rdmsr(msr);
}

// Wrapper for ops structure (needs 32-bit signature)
static void x2apic_write_op(uint32_t msr, uint32_t value) {
    ic_kapi->wrmsr(msr, value);
}
static uint32_t x2apic_read_op(uint32_t msr) {
    return (uint32_t)ic_kapi->rdmsr(msr);
}

uint32_t x2apic_get_id_raw(void) {
    // In x2APIC mode, the full 32-bit ID is available directly
    return (uint32_t)x2apic_read(X2APIC_ID);
}

static void x2apic_send_eoi_op(const uint32_t irn) { 
    (void)irn;
    ic_kapi->wrmsr(X2APIC_EOI, 0);
}

// Sends an Inter-Processor Interrupt (IPI) using x2APIC MSR
static void x2apic_send_ipi_op(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    irq_flags_t flags = ic_kapi->spinlock_lock_irqsave(&x2apic_ipi_lock);

    // In x2APIC mode, the ICR is a 64-bit MSR
    // Bits 0-7: Vector
    // Bits 8-10: Delivery Mode
    // Bits 11: Destination Mode (0=physical, 1=logical)
    // Bits 12-14: Reserved
    // Bits 15: Level (1=assert, 0=deassert)
    // Bits 16: Trigger Mode (0=edge, 1=level)
    // Bits 17-19: Reserved
    // Bits 20-31: Destination (APIC ID in x2APIC mode)
    // Bits 32-63: Reserved - WAIT, bit 32 is reserved?
    // x2APIC Spec: 
    // 31:0  - Low 32 bits
    // 63:32 - Destination Field (32-bit ID)
    
    // My previous assumption in xapic.c about bit shifts:
    // xAPIC: Destination at 56 (8 bits).
    // x2APIC: Destination at 32 (32 bits).
    
    // Check Intel SDM Vol 3A 10-28 "Interrupt Command Register (x2APIC Mode)"
    // 63:32 - Destination Field.
    // 31:0 - Vector, Delivery Mode, etc.
    
    uint64_t icr_value = (uint64_t)vector | 
                         (uint64_t)delivery_mode | 
                         (1ULL << 14) /* Reserved */ | 
                         (1ULL << 15) /* Assert Level */ | 
                         (0ULL << 16) /* Edge Trigger */ |
                         ((uint64_t)dest_apic_id << 32);

    // Wait for previous IPI to be delivered?
    // x2APIC ICR writes are serialized. But checking delivery status might still be relevant?
    // "Delivery Status (Bit 12) - Reserved in x2APIC mode."
    // So we do NOT check bit 12 in x2APIC mode!
    // The hardware handles buffering. 
    
    // Remove the wait loop for x2APIC!
    
    x2apic_write(X2APIC_ICR, icr_value);

    // No wait loop after write either.
    ic_kapi->spinlock_unlock_irqrestore(&x2apic_ipi_lock, flags);
}

// Initialize x2APIC by enabling x2APIC mode in the APIC_BASE MSR
static int x2apic_init_lapic(void) {
    uint64_t lapic_base_msr = ic_kapi->rdmsr(APIC_BASE_MSR);
    
    // Check if x2APIC is supported by checking CPUID
    uint32_t eax, ebx, ecx, edx;
    ic_kapi->cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 21))) { // Check for x2APIC feature bit (ECX bit 21)
        ic_kapi->printk(KERN_ERR APIC_CLASS  "x2APIC feature not supported by CPU\n");
        return 0;
    }
    
    ic_kapi->printk(KERN_DEBUG APIC_CLASS "Enabling x2APIC mode\n");

    // Enable x2APIC mode: set both APIC Global Enable and x2APIC Enable bits
    ic_kapi->wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE);

    // Add a small delay to ensure x2APIC is ready before register access
    // Different emulators have different timing requirements
    for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("nop" ::: "memory");
    }

    // Verify x2APIC is enabled by reading the version register
    uint64_t version = x2apic_read(X2APIC_VERSION);
    if ((version & 0xFF) == 0 || (version & 0xFF) == 0xFF) {
        ic_kapi->printk(KERN_ERR APIC_CLASS "x2APIC not responding after enable (version: 0x%llx)\n", version & 0xFF);
        return 0;
    }

    ic_kapi->printk(KERN_DEBUG APIC_CLASS "x2APIC Version: 0x%llx\n", version & 0xFF);

    // Set Spurious Interrupt Vector (0xFF) and enable APIC (bit 8)
    x2apic_write(X2APIC_SVR, 0x1FF);

    // Set TPR to 0 to accept all interrupts
    x2apic_write(X2APIC_TPR, 0);

    return 1;
}

static void x2apic_timer_set_frequency_op(uint32_t ticks_per_target) {
    if (ticks_per_target == 0) return;

    // First mask the timer to prevent spurious interrupts during configuration
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16)); // Masked

    // Set the divisor before initializing the count
    x2apic_write(X2APIC_TIMER_DIV, 0x3); // Divide by 16

    // Set the initial count (this also resets the current count)
    x2apic_write(X2APIC_TIMER_INIT_CNT, ticks_per_target);

    // Start Timer: Periodic, Interrupt Vector 32, Unmasked
    uint32_t lvt_timer = 32 | (1 << 17) | (0 << 16); // Vector 32, Periodic mode, Unmasked
    x2apic_write(X2APIC_LVT_TIMER, lvt_timer);

    ic_kapi->printk(KERN_DEBUG APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u, Div=0x3\n", lvt_timer, ticks_per_target);
}

static void x2apic_shutdown_op(void) {
    // 2. Disable Local APIC Timer
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16)); // Masked

    // 3. Disable Local APIC via SVR (clear bit 8)
    uint64_t svr = x2apic_read(X2APIC_SVR);
    x2apic_write(X2APIC_SVR, svr & ~(1ULL << 8));

    // 4. Disable x2APIC mode via MSR
    uint64_t lapic_base_msr = ic_kapi->rdmsr(APIC_BASE_MSR);
    ic_kapi->wrmsr(APIC_BASE_MSR, lapic_base_msr & ~(APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE));
}

const struct apic_ops x2apic_ops = {
    .name = "x2APIC",
    .init_lapic = x2apic_init_lapic,
    .send_eoi = x2apic_send_eoi_op,
    .send_ipi = x2apic_send_ipi_op,
    .get_id = x2apic_get_id_raw,
    .timer_init = NULL,
    .timer_set_frequency = x2apic_timer_set_frequency_op,
    .shutdown = x2apic_shutdown_op,
    .read = x2apic_read_op,
    .write = x2apic_write_op
};
