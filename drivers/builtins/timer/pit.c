/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/timer/pit.c
 * @brief PIT driver
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/export.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/io.h>
#include <arch/x86_64/tsc.h>
#include <drivers/timer/pit.h>

void __i_pit_wait_internal(uint32_t ms) {
  irq_flags_t flags = save_irq_flags();

  while (ms > 0) {
    uint32_t chunk_ms = (ms > 50) ? 50 : ms;
    ms -= chunk_ms;

    uint16_t count = (uint16_t)((PIT_FREQUENCY_BASE * chunk_ms) / 1000);

    outb(PIT_CMD_PORT, 0x30);
    io_wait();
    outb(PIT_CH0_PORT, count & 0xFF);
    io_wait();
    outb(PIT_CH0_PORT, (count >> 8) & 0xFF);

    while (1) {
      outb(PIT_CMD_PORT, 0x00);
      io_wait();
      uint8_t lo = inb(PIT_CH0_PORT);
      io_wait();
      uint8_t hi = inb(PIT_CH0_PORT);
      uint16_t current_val = ((uint16_t)hi << 8) | lo;

      if (current_val == 0 || current_val > count) {
        break;
      }
      cpu_relax();
    }
  }
  restore_irq_flags(flags);
}

int __i_pit_source_calibrate_tsc(void) {
  uint64_t start = rdtsc();
  __i_pit_wait_internal(50);
  uint64_t end = rdtsc();

  uint64_t freq = (end - start) * 20;
  tsc_recalibrate_with_freq(freq);
  return 0;
}