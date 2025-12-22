/// SPDX-License-Identifier: GPL-2.0-only
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
#include <arch/x64/io.h>
#include <arch/x64/mm/paging.h>
#include <arch/x64/smp.h>
#include <drivers/apic/apic_internal.h>
#include <drivers/apic/xapic.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <mm/vmalloc.h>

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
  uint64_t lapic_base_msr = rdmsr(APIC_BASE_MSR);

  if (xapic_lapic_base) {
    // Already mapped by BSP or another CPU, just enable for this core
    wrmsr(APIC_BASE_MSR, lapic_base_msr | APIC_BASE_MSR_ENABLE);
    xapic_write(XAPIC_SVR, 0x1FF); // 0xFF vector + bit 8 enabled
    xapic_write(XAPIC_TPR, 0);     // Accept all interrupts
    return 1;
  }

  // Get LAPIC physical base address from MSR
  uint64_t lapic_phys_base = lapic_base_msr & 0xFFFFFFFFFFFFF000ULL;

  // Prefer MADT LAPIC Address Override if present
  // Accessed via global from apic.c (extern in xapic.h)
  if (xapic_madt_parsed && xapic_madt_lapic_override_phys) {
    lapic_phys_base = (uint64_t)xapic_madt_lapic_override_phys;
    printk(APIC_CLASS "LAPIC base overridden by MADT: 0x%llx\n",
           lapic_phys_base);
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

  // Add a small delay to ensure APIC is ready before register access
  // Different emulators have different timing requirements
  for (volatile int i = 0; i < 1000; i++) {
    __asm__ volatile("nop" ::: "memory");
  }

  // Verify that we can read the version register to confirm APIC is working
  uint32_t version = xapic_read(XAPIC_VER);
  if (version == 0 || version == 0xFFFFFFFF) {
    printk(KERN_ERR APIC_CLASS
           "APIC not responding after enable (version: 0x%x)\n",
           version);
    return 0;
  }

  printk(APIC_CLASS "LAPIC Version: 0x%x\n", version);

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

static void xapic_send_ipi_op(uint32_t dest_apic_id, uint8_t vector,
                              uint32_t delivery_mode) {
  irq_flags_t flags = spinlock_lock_irqsave(&xapic_ipi_lock);

  // Wait for previous IPI to be delivered (ICR idle)
  uint32_t timeout = 100000;
  while (xapic_read(XAPIC_ICR_LOW) & (1 << 12)) {
    if (--timeout == 0) {
      printk(KERN_ERR APIC_CLASS "ICR stuck busy before send (dest: %u)\n",
             dest_apic_id);
      spinlock_unlock_irqrestore(&xapic_ipi_lock, flags);
      return;
    }
    cpu_relax();
  }

  // Write Destination APIC ID to ICR_HIGH
  xapic_write(XAPIC_ICR_HIGH, (uint32_t)dest_apic_id << 24);

  // Write Vector, Delivery Mode, Destination Mode (Physical), Level (Assert),
  // Trigger Mode (Edge) to ICR_LOW
  uint32_t icr_low = (uint32_t)vector | delivery_mode |
                     (1 << 14) /* Assert Level */ |
                     (0 << 15) /* Edge Trigger */;
  xapic_write(XAPIC_ICR_LOW, icr_low);

  // Wait for delivery to start/finish (Delivery Status bit 12 to be 0)
  timeout = 100000; // ~1 second timeout
  while (xapic_read(XAPIC_ICR_LOW) & (1 << 12)) {
    if (--timeout == 0) {
      printk(KERN_ERR APIC_CLASS "IPI delivery timeout to APIC ID %u\n",
             dest_apic_id);
      break;
    }
    cpu_relax();
  }

  spinlock_unlock_irqrestore(&xapic_ipi_lock, flags);
}

static void xapic_timer_set_frequency_op(uint32_t ticks_per_target) {
  if (ticks_per_target == 0)
    return;

  // Start Timer: Periodic, Interrupt Vector 32, Unmasked
  uint32_t lvt_timer = 32 | (1 << 17); // Vector 32, Periodic mode
  xapic_write(XAPIC_LVT_TIMER, lvt_timer);
  xapic_write(XAPIC_TIMER_DIV, 0x3); // Divide by 16
  xapic_write(XAPIC_TIMER_INIT_COUNT, ticks_per_target);

  printk(APIC_CLASS "Timer configured: LVT=0x%x, Ticks=%u\n", lvt_timer,
         ticks_per_target);
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

const struct apic_ops xapic_ops = {.name = "xAPIC",
                                   .init_lapic = xapic_init_lapic,
                                   .send_eoi = xapic_send_eoi_op,
                                   .send_ipi = xapic_send_ipi_op,
                                   .get_id = xapic_get_id_raw,
                                   .timer_init = NULL,
                                   .timer_set_frequency =
                                       xapic_timer_set_frequency_op,
                                   .shutdown = xapic_shutdown_op,
                                   .read = xapic_read,
                                   .write = xapic_write};
