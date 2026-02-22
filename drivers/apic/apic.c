///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/apic/apic.c
 * @brief APIC abstraction system
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/io.h>
#include <drivers/apic/apic_internal.h>
#include <drivers/apic/ioapic.h>
#include <drivers/apic/pic.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/madt.h>
#include <acpi.h>
#include <arch/x86_64/cpu.h>
#include <lib/printk.h>

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

// --- Global Variables ---
static const struct apic_ops *current_ops = nullptr;
static uint32_t apic_calibrated_ticks = 0;

// --- Forward Declarations ---
static int detect_apic(void);
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

// --- Core APIC Functions (Abstraction Layer) ---

int apic_init(void) {
  pic_mask_all();

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

  size_t num_ioapics;
  const madt_ioapic_t *ioapics = madt_get_ioapics(&num_ioapics);

  uint64_t ioapic_phys = (num_ioapics > 0)
                           ? (uint64_t) ioapics[0].address
                           : (uint64_t) IOAPIC_DEFAULT_PHYS_ADDR;

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
    current_ops->send_ipi((uint32_t) dest_apic_id, vector, delivery_mode);
  }
}

uint8_t lapic_get_id(void) {
  if (current_ops && current_ops->get_id) {
    return (uint8_t) current_ops->get_id();
  }
  return 0;
}

void apic_enable_irq(uint32_t irq_line) {
  uint32_t dest_apic_id = 0;
  if (current_ops && current_ops->get_id) {
    dest_apic_id = current_ops->get_id();
  }

  uint32_t gsi = irq_line;
  uint16_t flags = 0;
  bool found_override = false;

  size_t num_overrides;
  const madt_iso_t *overrides = madt_get_overrides(&num_overrides);

  for (size_t i = 0; i < num_overrides; i++) {
    if (overrides[i].source == (uint8_t)irq_line) {
      gsi = overrides[i].gsi;
      flags = overrides[i].flags;
      found_override = true;
      break;
    }
  }

  // If no override found, set defaults based on GSI
  if (!found_override) {
    if (gsi >= 16) {
      // PCI/System interrupts are Level/Low by default
      flags = ACPI_MADT_POLARITY_ACTIVE_LOW | ACPI_MADT_TRIGGER_LEVEL;
    } else {
      // ISA interrupts are Edge/High by default
      flags = ACPI_MADT_POLARITY_ACTIVE_HIGH | ACPI_MADT_TRIGGER_EDGE;
    }
  }

  int is_x2apic = (current_ops == &x2apic_ops);
  ioapic_set_gsi_redirect(gsi, 32 + (uint8_t)irq_line, dest_apic_id, flags, 0, is_x2apic);
}

void apic_disable_irq(uint32_t irq_line) {
  uint32_t gsi = irq_line;
  size_t num_overrides;
  const madt_iso_t *overrides = madt_get_overrides(&num_overrides);

  for (size_t i = 0; i < num_overrides; i++) {
    if (overrides[i].source == (uint8_t)irq_line) {
      gsi = overrides[i].gsi;
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

  // Use PIT Channel 2 for calibration
  uint16_t pit_reload = 11931; // ~10ms at 1193180 Hz
  
  // Set PIT Channel 2 to Mode 0 (Interrupt on Terminal Count), Lo/Hi access
  outb(0x43, 0xB0);
  outb(0x42, pit_reload & 0xFF);
  outb(0x42, (pit_reload >> 8) & 0xFF);

  // Ensure Gate 2 is enabled so the counter actually runs
  outb(0x61, (inb(0x61) & 0xFD) | 0x01);

  current_ops->write(regs->init_count, 0xFFFFFFFF);

  // Poll the PIT counter using the Latch command.
  uint32_t safety_timeout = 1000000;
  while (safety_timeout--) {
    outb(0x43, 0x80); // Latch Channel 2
    uint8_t lo = inb(0x42);
    uint8_t hi = inb(0x42);
    uint16_t count = (hi << 8) | lo;
    
    if (count == 0 || count > pit_reload)
      break;
    
    cpu_relax();
  }

  if (safety_timeout == 0) {
    apic_calibrated_ticks = 100000; // Reasonable default for 10ms
    printk(KERN_NOTICE APIC_CLASS "Timer calibration timeout, using default: %u ticks in 10ms.\n",
           apic_calibrated_ticks);
  } else {
    apic_calibrated_ticks = 0xFFFFFFFF - current_ops->read(regs->cur_count);
    printk(KERN_DEBUG APIC_CLASS "Calibrated timer: %u ticks in 10ms.\n", apic_calibrated_ticks);
  }

  // Additional safety check
  if (apic_calibrated_ticks < 1000 || apic_calibrated_ticks > 10000000) {
    printk(KERN_NOTICE APIC_CLASS "Calibration result unreasonable (%u), using default.\n", apic_calibrated_ticks);
    apic_calibrated_ticks = 100000; // Reasonable default
  }
}

static int detect_tsc_deadline(void) {
  uint32_t eax, ebx, ecx, edx;
  cpuid(1, &eax, &ebx, &ecx, &edx);
  return (ecx & (1 << 24)) != 0;
}

int apic_has_tsc_deadline(void) {
  return detect_tsc_deadline();
}

uint32_t apic_get_calibrated_ticks(void) {
  return apic_calibrated_ticks;
}

void apic_timer_stop(void) {
  if (current_ops && current_ops->timer_stop)
    current_ops->timer_stop();
}

void apic_timer_set_oneshot(uint32_t microseconds) {
  if (!current_ops || !current_ops->timer_set_oneshot) return;

  if (apic_calibrated_ticks == 0) {
    current_ops->timer_set_oneshot(microseconds * 100);
    return;
  }

  uint64_t ticks = ((uint64_t) apic_calibrated_ticks * microseconds) / 10000;
  if (ticks > 0xFFFFFFFF) ticks = 0xFFFFFFFF;
  if (ticks == 0) ticks = 1;

  current_ops->timer_set_oneshot((uint32_t) ticks);
}

void apic_timer_set_periodic(uint32_t frequency_hz) {
  if (frequency_hz == 0) return;

  uint32_t ticks_per_target;
  if (apic_calibrated_ticks == 0) {
    ticks_per_target = 1000000 / frequency_hz;
  } else {
    ticks_per_target = (apic_calibrated_ticks * 100) / frequency_hz;
  }

  if (current_ops && current_ops->timer_set_periodic) {
    current_ops->timer_set_periodic(ticks_per_target);
  }
}

void apic_timer_set_tsc_deadline(uint64_t tsc_deadline) {
  if (current_ops && current_ops->timer_set_tsc_deadline && detect_tsc_deadline()) {
    current_ops->timer_set_tsc_deadline(tsc_deadline);
  }
}

void apic_timer_set_frequency(uint32_t frequency_hz) {
  apic_timer_set_periodic(frequency_hz);
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
  .timer_set = apic_timer_set_periodic,
  .timer_stop = apic_timer_stop,
  .timer_oneshot = apic_timer_set_oneshot,
  .timer_tsc_deadline = apic_timer_set_tsc_deadline,
  .timer_has_tsc_deadline = apic_has_tsc_deadline,
  .enable_irq = apic_enable_irq,
  .disable_irq = apic_disable_irq,
  .send_eoi = apic_send_eoi,
  .mask_all = apic_mask_all,
  .shutdown = apic_shutdown,
  .priority = 100,
  .send_ipi = apic_send_ipi,
  .get_id = lapic_get_id,
};

const interrupt_controller_interface_t *apic_get_driver(void) {
  return &apic_interface;
}

#include <aerosync/export.h>
EXPORT_SYMBOL(apic_get_driver);
EXPORT_SYMBOL(apic_send_eoi);
EXPORT_SYMBOL(lapic_get_id);
