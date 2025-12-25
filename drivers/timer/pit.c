/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file drivers/timer/pit.c
 * @brief PIT driver
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
#include <arch/x64/io.h>
#include <arch/x64/tsc.h>
#include <drivers/timer/pit.h>
#include <kernel/sysintf/time.h>

#define PIT_CMD_PORT 0x43
#define PIT_CH0_PORT 0x40
#define PIT_CH1_PORT 0x41
#define PIT_CH2_PORT 0x42

static uint32_t global_pit_frequency = 100; // Default
static uint16_t pit_reload_value = 0;

void pit_set_frequency(uint32_t frequency) {
  if (frequency == 0)
    frequency = 100; // Safe default
  if (frequency > PIT_FREQUENCY_BASE)
    frequency = PIT_FREQUENCY_BASE;

  global_pit_frequency = frequency;

  uint32_t divisor = PIT_FREQUENCY_BASE / frequency;
  if (divisor > 65535)
    divisor = 65535;

  pit_reload_value = (uint16_t)divisor;

  // Save IRQ state
  irq_flags_t flags = save_irq_flags();

  // Mode 2 (Rate Generator), Binary, Channel 0
  // 00 11 010 0 -> 0x34
  outb(PIT_CMD_PORT, 0x34);
  outb(PIT_CH0_PORT, divisor & 0xFF);
  outb(PIT_CH0_PORT, (divisor >> 8) & 0xFF);

  restore_irq_flags(flags);
}

// Internal wait function (keeps existing logic but private/adapted)
static void pit_wait_internal(uint32_t ms) {
  irq_flags_t flags = save_irq_flags();

  while (ms > 0) {
    uint32_t chunk_ms = (ms > 50) ? 50 : ms;
    ms -= chunk_ms;

    // Calculate count for this chunk
    uint16_t count = (uint16_t)((PIT_FREQUENCY_BASE * chunk_ms) / 1000);

    // Mode 0: Interrupt on Terminal Count
    outb(PIT_CMD_PORT, 0x30);
    outb(PIT_CH0_PORT, count & 0xFF);
    outb(PIT_CH0_PORT, (count >> 8) & 0xFF);

    while (1) {
      // Latch Counter 0
      outb(PIT_CMD_PORT, 0x00);
      uint8_t lo = inb(PIT_CH0_PORT);
      uint8_t hi = inb(PIT_CH0_PORT);
      uint16_t current_val = ((uint16_t)hi << 8) | lo;

      if (current_val == 0 || current_val > count) {
        break;
      }

      cpu_relax();
    }
  }

  // Restore generic frequency
  pit_set_frequency(global_pit_frequency);

  restore_irq_flags(flags);
}

// Time Source Interface Implementation

static int pit_source_init(void) {
  // PIT is usually always available and initialized early, but we can reset it
  // here
  pit_set_frequency(100);
  return 0;
}

static uint64_t pit_source_get_frequency(void) { return PIT_FREQUENCY_BASE; }

static uint64_t pit_source_read_counter(void) {
  // Reading PIT counter on the fly is tricky because it's a down counter
  // and typically runs in Mode 2 (Rate Generator) repeating 0..Reload..0
  // To implement a monotonic up-counter we would need to track overflows
  // (interrupts). For simple short waits (calibration), we can just read the
  // down counter and handle logic. BUT `read_counter` implies a continuous
  // counter. PIT is essentially 16-bit.

  // For now, let's implement a simple read that just returns the inverted
  // current count so it looks like it's going UP within one period. NOTE: This
  // assumes we are within one tick (frequency dependent). This is NOT
  // sufficient for long waits without ISR support.

  // Better approach for unification:
  // If we use PIT for *system* timer, we rely on Jiffies/Ticks for long time.
  // But `time_wait_ns` wants high precision.

  // Latch and read
  irq_flags_t flags = save_irq_flags();
  outb(PIT_CMD_PORT, 0x00);
  uint8_t lo = inb(PIT_CH0_PORT);
  uint8_t hi = inb(PIT_CH0_PORT);
  restore_irq_flags(flags);

  uint16_t count = ((uint16_t)hi << 8) | lo;

  // Convert down-counter to up-counting value within the period
  return (uint64_t)(pit_reload_value - count);
}

static int pit_source_calibrate_tsc(void) {
  // Use the existing busy-wait logic which is robust for PIT
  uint64_t start = rdtsc();
  pit_wait_internal(50); // 50ms
  uint64_t end = rdtsc();

  uint64_t freq = (end - start) * 20;
  tsc_recalibrate_with_freq(freq);
  return 0;
}

static time_source_t pit_time_source = {
    .name = "PIT",
    .priority = 100, // Low priority
    .type = TIME_SOURCE_PIT,
    .init = pit_source_init,
    .get_frequency = pit_source_get_frequency,
    .read_counter = pit_source_read_counter,
    .calibrate_tsc = pit_source_calibrate_tsc,
};

// Direct calibration function exposed for early boot
void pit_calibrate_tsc(void) { pit_source_calibrate_tsc(); }

// Make sure this is called during system init
__attribute__((constructor)) static void register_pit_source(void) {
  time_register_source(&pit_time_source);
}

// Getter for manual registration (Time Subsystem)
const time_source_t *pit_get_time_source(void) { return &pit_time_source; }

// Keep the old symbol for compatibility if needed, but it's better to use the
// new interface.
void pit_wait(uint32_t ms) {
  // Redirect to internal implementation or new system?
  // If new system is up, use it?
  // But pit_wait is specific.
  pit_wait_internal(ms);
}
