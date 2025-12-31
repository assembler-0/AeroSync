///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/x2apic.c
 * @brief x2APIC driver
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/classes.h> 
#include <drivers/apic/x2apic.h> 
#include <drivers/apic/apic_internal.h> 
#include <kernel/fkx/fkx.h> 
#include <arch/x64/cpu.h> 
#include <lib/printk.h> 
#include <kernel/spinlock.h> 

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

#define APIC_BASE_MSR 0x1B
#define APIC_BASE_MSR_ENABLE 0x800
#define APIC_BASE_MSR_X2APIC_ENABLE (1ULL << 10)

static spinlock_t x2apic_ipi_lock = 0;

static void x2apic_write(uint32_t msr, uint64_t value) {
    wrmsr(msr, value);
}

static uint64_t x2apic_read(uint32_t msr) {
    return rdmsr(msr);
}

static void x2apic_write_op(uint32_t msr, uint32_t value) {
    wrmsr(msr, value);
}
static uint32_t x2apic_read_op(uint32_t msr) {
    return (uint32_t)rdmsr(msr);
}

uint32_t x2apic_get_id_raw(void) {
    return (uint32_t)x2apic_read(X2APIC_ID);
}

static void x2apic_send_eoi_op(const uint32_t irn) { 
    (void)irn;
    wrmsr(X2APIC_EOI, 0);
}

static void x2apic_send_ipi_op(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
    irq_flags_t flags = spinlock_lock_irqsave(&x2apic_ipi_lock);

    uint64_t icr_value = (uint64_t)vector | 
                         (uint64_t)delivery_mode | 
                         (1ULL << 14) | 
                         (1ULL << 15) | 
                         (0ULL << 16) |
                         ((uint64_t)dest_apic_id << 32);
    
    x2apic_write(X2APIC_ICR, icr_value);
    spinlock_unlock_irqrestore(&x2apic_ipi_lock, flags);
}

static int x2apic_init_lapic(void) {
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1 << 21))) {
        printk(KERN_ERR APIC_CLASS  "x2APIC feature not supported by CPU\n");
        return 0;
    }
    
    printk(KERN_DEBUG APIC_CLASS "Enabling x2APIC mode\n");
    wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE);

    for (volatile int i = 0; i < 1000; i++) {
        __asm__ volatile("nop" ::: "memory");
    }

    uint64_t version = x2apic_read(X2APIC_VERSION);
    if ((version & 0xFF) == 0 || (version & 0xFF) == 0xFF) {
        printk(KERN_ERR APIC_CLASS "x2APIC not responding after enable (version: 0x%llx)\n", version & 0xFF);
        return 0;
    }

    printk(KERN_DEBUG APIC_CLASS "x2APIC Version: 0x%llx\n", version & 0xFF);
    x2apic_write(X2APIC_SVR, 0x1FF);
    x2apic_write(X2APIC_TPR, 0);
    return 1;
}

static void x2apic_timer_set_frequency_op(uint32_t ticks_per_target) {
    if (ticks_per_target == 0) return;
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16));
    x2apic_write(X2APIC_TIMER_DIV, 0x3);
    x2apic_write(X2APIC_TIMER_INIT_CNT, ticks_per_target);
    uint32_t lvt_timer = 32 | (1 << 17) | (0 << 16);
    x2apic_write(X2APIC_LVT_TIMER, lvt_timer);

    printk(KERN_DEBUG APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u, Div=0x3\n", lvt_timer, ticks_per_target);
}

static void x2apic_shutdown_op(void) {
    x2apic_write(X2APIC_LVT_TIMER, (1 << 16));
    uint64_t svr = x2apic_read(X2APIC_SVR);
    x2apic_write(X2APIC_SVR, svr & ~(1ULL << 8));
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    wrmsr(APIC_BASE_MSR, lapic_base_msr & ~(APIC_BASE_MSR_ENABLE | APIC_BASE_MSR_X2APIC_ENABLE));
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