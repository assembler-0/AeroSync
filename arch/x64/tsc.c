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

#include <arch/x64/tsc.h>
#include <drivers/timer/time.h> // Use Unified Time Subsystem
#include <kernel/classes.h>
#include <printk.h>

static uint64_t tsc_freq = 0;
static uint64_t tsc_boot_offset = 0;

uint64_t tsc_freq_get() {
  if (tsc_freq == 0) {
    // Try system calibration first (if time subsystem is up)
    if (time_calibrate_tsc_system() != 0) {

      // If still 0, we are in trouble.
      if (tsc_freq == 0) {
        // Only print if robust enough, or risk infinite recursion if printk
        // needs time? Assuming printk is safe if we don't crash here. Set a
        // fallback immediately to avoid div-by-zero in printk's timestamping
        tsc_freq = 2500000000ULL; // 2.5 GHz safe default/fallback
        tsc_boot_offset = rdtsc();

        printk(KERN_WARNING TSC_CLASS
               "TSC calibration failed, assuming 2.5 GHz\n");
      }
    }
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

#include <drivers/timer/pit.h> // for pit_calibrate_tsc fallback

uint64_t get_time_ns() {
  if (tsc_freq == 0)
    return 0; // Guard against divide-by-zero
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