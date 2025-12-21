/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/tsc.c
 * @brief TSC (Time Stamp Counter) management and calibration
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

#include <printk.h>
#include <arch/x64/tsc.h>
#include <drivers/timer/pit.h>
#include <kernel/classes.h>

static uint64_t tsc_freq = 0;
static uint64_t tsc_boot_offset = 0;

uint64_t tsc_freq_get() {
  // Use PIT specific calibration
  // We don't rely on timer_hz_before from argument fr APIC anymore,
  // we specificially use the PIT for calibration block.

  if (tsc_freq == 0) {
    uint64_t start = rdtsc();
    pit_wait(50); // Wait 50ms using PIT busy-wait
    uint64_t end = rdtsc();

    // delta is 50ms worth of ticks
    // freq = delta * (1000 / 50) = delta * 20
    tsc_freq = (end - start) * 20;
    tsc_boot_offset = end; // Use 'end' as the zero point (roughly)
  }

  return tsc_freq;
}

uint64_t get_tsc_freq(void) { return tsc_freq; }

uint64_t calibrate_tsc(void) { return tsc_freq_get(); }

void tsc_recalibrate_with_freq(uint64_t new_freq) {
    if (new_freq > 0) {
        tsc_freq = new_freq;
        printk(TSC_CLASS "TSC recalibrated to %lu Hz\n", tsc_freq);
    }
}

uint64_t get_time_ns() {
  uint64_t now = rdtsc();
  if (now < tsc_boot_offset)
    return 0;
  return ((now - tsc_boot_offset) * 1000000000ULL) / tsc_freq;
}

uint64_t rdtsc(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

uint64_t rdtscp(void) {
  uint32_t lo, hi;
  __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi)::"rcx");
  return ((uint64_t)hi << 32) | lo;
}

void tsc_delay(uint64_t ns) {
  uint64_t start = rdtsc();
  uint64_t end;
  uint64_t ticks = (tsc_freq * ns) / 1000000000ULL;
  do {
    end = rdtsc();
  } while ((end - start) < ticks);
}

void tsc_delay_ms(uint64_t ms) { tsc_delay(ms * 1000000ULL); }