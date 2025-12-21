///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/apic/xapic.c
 * @brief xAPIC (x86 Advanced Programmable Interrupt Controller) driver
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
#include <drivers/apic/xapic.h>
#include <drivers/apic/apic_internal.h>

// --- Register Definitions ---

// Local APIC registers (offsets from LAPIC base)
#define XAPIC_ID 0x0020               // LAPIC ID
#define XAPIC_VER 0x0030              // LAPIC Version
#define XAPIC_TPR 0x0080              // Task Priority
#define XAPIC_EOI 0x00B0              // EOI
#define XAPIC_LDR 0x00D0              // Logical Destination
#define XAPIC_DFR 0x00E0              // Destination Format
#define XAPIC_SVR 0x00F0              // Spurious Interrupt Vector
#define XAPIC_ESR 0x0280              // Error Status
#define XAPIC_ICR_LOW 0x0300          // Interrupt Command Reg low
#define XAPIC_ICR_HIGH 0x0310         // Interrupt Command Reg high
#define XAPIC_LVT_TIMER 0x0320        // LVT Timer
#define XAPIC_LVT_LINT0 0x0350        // LVT LINT0
#define XAPIC_LVT_LINT1 0x0360        // LVT LINT1
#define XAPIC_LVT_ERROR 0x0370        // LVT Error
#define XAPIC_TIMER_INIT_COUNT 0x0380 // Initial Count (for Timer)
#define XAPIC_TIMER_CUR_COUNT 0x0390  // Current Count (for Timer)
#define XAPIC_TIMER_DIV 0x03E0        // Divide Configuration

// --- Constants ---
#define APIC_BASE_MSR 0x1B
#define APIC_BASE_MSR_ENABLE 0x800

// --- Globals ---
volatile uint32_t *xapic_lapic_base = NULL;
static spinlock_t xapic_ipi_lock;
static volatile uint32_t xapic_timer_hz = 0;

// --- MMIO Helper Functions ---

static void xapic_write(uint32_t reg, uint32_t value) {
  if (xapic_lapic_base)
    xapic_lapic_base[reg / 4] = value;
}

static uint32_t xapic_read(uint32_t reg) {
  if (!xapic_lapic_base)
    return 0;
  return xapic_lapic_base[reg / 4];
}

uint32_t xapic_get_id_raw(void) {
  // LAPIC_ID register: bits 24..31 hold the APIC ID in xAPIC mode
  return (uint32_t)(xapic_read(XAPIC_ID) >> 24);
}

// --- xAPIC Functions ---

static int xapic_init_lapic(void) {
  // Get LAPIC physical base address from MSR
  uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
  uint64_t lapic_phys_base = lapic_base_msr & 0xFFFFFFFFFFFFF000ULL;

  // Prefer MADT LAPIC Address Override if present
  // Accessed via global from apic.c (extern in xapic.h)
  if (xapic_madt_parsed && xapic_madt_lapic_override_phys) {
    lapic_phys_base = (uint64_t)xapic_madt_lapic_override_phys;
    printk(APIC_CLASS "LAPIC base overridden by MADT: 0x%llx\n", lapic_phys_base);
  }

  printk(APIC_CLASS "LAPIC Physical Base: 0x%llx\n", lapic_phys_base);

  // Map the LAPIC into virtual memory
  xapic_lapic_base = (volatile uint32_t *)viomap(lapic_phys_base, PAGE_SIZE);

  if (!xapic_lapic_base) {
    printk(KERN_ERR APIC_CLASS "Failed to map LAPIC MMIO.\n");
    return 0;
  }

  printk(APIC_CLASS "LAPIC Mapped at: 0x%llx\n", (uint64_t)xapic_lapic_base);

  // Enable the LAPIC
  wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE);

  // Set Spurious Interrupt Vector (0xFF) and enable APIC (bit 8)
  xapic_write(XAPIC_SVR, 0x1FF);

  // Set TPR to 0 to accept all interrupts
  xapic_write(XAPIC_TPR, 0);
  return 1;
}

static void xapic_send_eoi_op(uint32_t irn) { 
  (void)irn;
  xapic_write(XAPIC_EOI, 0);
}

static void xapic_send_ipi_op(uint32_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
  irq_flags_t flags = spinlock_lock_irqsave(&xapic_ipi_lock);

  // Wait for previous IPI to be delivered (ICR idle)
  uint32_t timeout = 100000;
  while (xapic_read(XAPIC_ICR_LOW) & (1 << 12)) {
    if (--timeout == 0) {
      printk(KERN_ERR APIC_CLASS "ICR stuck busy before send (dest: %u)\n", dest_apic_id);
      spinlock_unlock_irqrestore(&xapic_ipi_lock, flags);
      return;
    }
    cpu_relax();
  }

  // Write Destination APIC ID to ICR_HIGH
  xapic_write(XAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);

  // Write Vector, Delivery Mode, Destination Mode (Physical), Level (Assert), Trigger Mode (Edge) to ICR_LOW
  uint32_t icr_low = (uint32_t)vector | delivery_mode | (1 << 14) /* Assert Level */ | (0 << 15) /* Edge Trigger */;
  xapic_write(XAPIC_ICR_LOW, icr_low);

  // Wait for delivery to start/finish (Delivery Status bit 12 to be 0)
  timeout = 100000; // ~1 second timeout
  while (xapic_read(XAPIC_ICR_LOW) & (1 << 12)) {
    if (--timeout == 0) {
      printk(KERN_ERR APIC_CLASS "IPI delivery timeout to APIC ID %u\n", dest_apic_id);
      break;
    }
    cpu_relax();
  }

  spinlock_unlock_irqrestore(&xapic_ipi_lock, flags);
}

static void xapic_timer_set_frequency_op(uint32_t frequency_hz) {
  if (frequency_hz == 0)
    return;

  xapic_timer_hz = frequency_hz;

  // Use PIT to calibrate
  // 1. Prepare LAPIC Timer: Divide by 16, One-shot, Masked
  xapic_write(XAPIC_TIMER_DIV, 0x3);       // Divide by 16
  xapic_write(XAPIC_LVT_TIMER, (1 << 16)); // Masked

  // 2. Prepare PIT: Channel 2, Mode 0, Rate = 11931 (approx 10ms)
  // 1193182 Hz / 11931 ~= 100 Hz (10ms)
  uint16_t pit_reload = 11931;
  outb(0x61, (inb(0x61) & 0xFD) | 1); // Gate high
  outb(0x43, 0xB0);                   // Channel 2, Access Lo/Hi, Mode 0, Binary
  outb(0x42, pit_reload & 0xFF);
  outb(0x42, (pit_reload >> 8) & 0xFF);

  // 3. Reset APIC Timer to -1
  xapic_write(XAPIC_TIMER_INIT_COUNT, 0xFFFFFFFF);

  // 4. Wait for PIT to wrap (10ms)
  while (!(inb(0x61) & 0x20))
    ;

  // 5. Stop APIC timer
  xapic_write(XAPIC_LVT_TIMER, (1 << 16));

  // 6. Calculate ticks
  uint32_t ticks_in_10ms = 0xFFFFFFFF - xapic_read(XAPIC_TIMER_CUR_COUNT);

  // 7. Calculate ticks per period for target frequency
  // ticks_in_10ms corresponds to 100Hz (0.01s)
  // ticks_per_sec = ticks_in_10ms * 100
  // target = ticks_per_sec / frequency_hz
  uint32_t ticks_per_target = (ticks_in_10ms * 100) / frequency_hz;

  // 8. Start Timer: Periodic, Interrupt Vector 32, Unmasked
  // Bit 17 = 1 for Periodic mode, Bit 16 = 0 for Unmasked
  uint32_t lvt_timer = 32 | (1 << 17); // Vector 32, Periodic mode, Unmasked
  xapic_write(XAPIC_LVT_TIMER, lvt_timer);
  xapic_write(XAPIC_TIMER_DIV, 0x3); // Divide by 16
  xapic_write(XAPIC_TIMER_INIT_COUNT, ticks_per_target);

  printk(APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u\n", lvt_timer,
         ticks_per_target);
}

static void xapic_timer_init_op(uint32_t frequency_hz) {
  xapic_timer_set_frequency_op(frequency_hz);
  printk(APIC_CLASS "Timer installed at %d Hz.\n", frequency_hz);
}

static void xapic_shutdown_op(void) {
    // 2. Disable Local APIC Timer
    xapic_write(XAPIC_LVT_TIMER, (1 << 16)); // Masked

    // 3. Disable Local APIC via SVR (clear bit 8)
    uint32_t svr = xapic_read(XAPIC_SVR);
    xapic_write(XAPIC_SVR, svr & ~(1 << 8));

    // 4. Optionally disable via MSR (APIC Global Enable)
    uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);
    wrmsr(APIC_BASE_MSR, lapic_base_msr & ~APIC_BASE_MSR_ENABLE);
}

const struct apic_ops xapic_ops = {
    .name = "xAPIC",
    .init_lapic = xapic_init_lapic,
    .send_eoi = xapic_send_eoi_op,
    .send_ipi = xapic_send_ipi_op,
    .get_id = xapic_get_id_raw,
    .timer_init = xapic_timer_init_op,
    .timer_set_frequency = xapic_timer_set_frequency_op,
    .shutdown = xapic_shutdown_op,
    .read = xapic_read,
    .write = xapic_write
};
