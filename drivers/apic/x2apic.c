/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/x2apic.c
 * @brief x2APIC driver
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 */

#include <arch/x64/cpu.h> 
#include <arch/x64/smp.h> 
#include <arch/x64/io.h> 
#include <arch/x64/mm/paging.h> 
#include <kernel/classes.h> 
#include <lib/printk.h> 
#include <mm/vmalloc.h> 
#include <kernel/spinlock.h> 
#include <drivers/apic/x2apic.h> 
#include <drivers/apic/apic_internal.h> 

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

// --- Globals ---
static spinlock_t x2apic_ipi_lock;
volatile uint32_t x2apic_timer_hz = 0;

// --- x2APIC MSR Access Functions ---

static void x2apic_write(uint32_t msr, uint64_t value) {
    wrmsr(msr, value);
}

static uint64_t x2apic_read(uint32_t msr) {
    return rdmsr(msr);
}

// Wrapper for ops structure (needs 32-bit signature)
static void x2apic_write_op(uint32_t msr, uint32_t value) {
    wrmsr(msr, value);
}
static uint32_t x2apic_read_op(uint32_t msr) {
    return (uint32_t)rdmsr(msr);
}

uint32_t x2apic_get_id_raw(void) {
    // In x2APIC mode, the full 32-bit ID is available directly
    return (uint32_t)x2apic_read(X2APIC_ID);
}

static void x2apic_send_eoi_op(const uint32_t irn) { 
    (void)irn;
    wrmsr(X2APIC_EOI, 0);
}

// Sends an Inter-Processor Interrupt (IPI) using x2APIC MSR
static void x2apic_send_ipi_op(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
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
    spinlock_unlock_irqrestore(&x2apic_ipi_lock, flags);
}

// Initialize x2APIC by enabling x2APIC mode in the APIC_BASE MSR
static int x2apic_init_lapic(void) {
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    
    // Check if x2APIC is supported by checking CPUID
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 21))) { // Check for x2APIC feature bit (ECX bit 21)
        printk(KERN_ERR APIC_CLASS  "x2APIC feature not supported by CPU\n");
        return 0;
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
    
    return 1;
}

static void x2apic_timer_set_frequency_op(uint32_t frequency_hz) {
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

static void x2apic_timer_init_op(uint32_t frequency_hz) {
    x2apic_timer_set_frequency_op(frequency_hz);
    printk(APIC_CLASS "Timer installed at %d Hz.\n", frequency_hz);
}

static void x2apic_shutdown_op(void) {
    // 2. Disable Local APIC Timer
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16)); // Masked

    // 3. Disable Local APIC via SVR (clear bit 8)
    uint64_t svr = x2apic_read(X2APIC_SVR);
    x2apic_write(X2APIC_SVR, svr & ~(1ULL << 8));

    // 4. Disable x2APIC mode via MSR
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    wrmsr(APIC_BASE_MSR, lapic_base_msr & ~(APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE));
}

const struct apic_ops x2apic_ops = {
    .name = "x2APIC",
    .init_lapic = x2apic_init_lapic,
    .send_eoi = x2apic_send_eoi_op,
    .send_ipi = x2apic_send_ipi_op,
    .get_id = x2apic_get_id_raw,
    .timer_init = x2apic_timer_init_op,
    .timer_set_frequency = x2apic_timer_set_frequency_op,
    .shutdown = x2apic_shutdown_op,
    .read = x2apic_read_op,
    .write = x2apic_write_op
};
