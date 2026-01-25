/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/tsc.c
 * @brief TSC (Time Stamp Counter) management and calibration
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/tsc.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <aerosync/fkx/fkx.h>

static uint64_t tsc_freq = 0;
static uint64_t tsc_boot_offset = 0;

void tsc_calibrate_early(void) {
  uint32_t eax, ebx, ecx, edx;

  /* ---------- Tier 1: CPUID 0x15 ---------- */
  cpuid(0x15, &eax, &ebx, &ecx, &edx);

  if (eax && ebx) {
    uint64_t crystal_hz = ecx;

    /* 
     * On many modern Intel CPUs (Skylake+), the crystal frequency is 24MHz 
     * but CPUID.15H:ECX might be 0. 
     */
    if (crystal_hz == 0) {
      /* 
       * Assume 24MHz as a safe default for modern Intel.
       * Some Atom CPUs use 19.2MHz, but Core CPUs use 24MHz.
       */
      crystal_hz = 24000000;
    }

    uint64_t tsc_hz = (crystal_hz * ebx) / eax;
    tsc_freq = tsc_hz;
    return;
  }

  /* ---------- Tier 2: CPUID 0x16 ---------- */
  cpuid(0x16, &eax, &ebx, &ecx, &edx);

  if (eax) {
    /* eax = base frequency in MHz */
    tsc_freq = (uint64_t)eax * 1000000;
    return;
  }

  /* ---------- Tier 3: Trust me bro fallback ---------- */
  /* Assume ~3GHz */
  tsc_freq = 3000000000;
}

uint64_t tsc_freq_get() {
  return tsc_freq;
}

void tsc_recalibrate_with_freq(uint64_t new_freq) {
  if (new_freq > 0) {
    tsc_freq = new_freq;
    printk(KERN_DEBUG TSC_CLASS "TSC recalibrated to %lu Hz\n", tsc_freq);
  }
}

#include <drivers/timer/pit.h> // for pit_calibrate_tsc fallback

uint64_t get_time_ns() {
  if (tsc_freq == 0)
    return 0;
  uint64_t now = rdtsc();
  if (now < tsc_boot_offset)
    return 0;

  uint64_t cycles = now - tsc_boot_offset;
  
  /* 
   * Use a more robust calculation to avoid overflow:
   * (cycles / freq) * 1e9 + ((cycles % freq) * 1e9) / freq
   */
  uint64_t seconds = cycles / tsc_freq;
  uint64_t remainder = cycles % tsc_freq;
  
  return (seconds * 1000000000ULL) + ((remainder * 1000000000ULL) / tsc_freq);
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
  
  /* 
   * Avoid overflow in (tsc_freq * ns) / 1e9 by using the same logic 
   * in reverse or using __int128 if available. 
   * Since we are in the kernel, we can use __int128 for simplicity if the compiler supports it.
   */
  unsigned __int128 ticks_128 = (unsigned __int128)tsc_freq * ns;
  uint64_t ticks = (uint64_t)(ticks_128 / 1000000000ULL);

  do {
    end = rdtsc();
  } while ((end - start) < ticks);
}

void tsc_delay_ms(uint64_t ms) { tsc_delay(ms * 1000000ULL); }
EXPORT_SYMBOL(rdtsc);
EXPORT_SYMBOL(rdtscp);
EXPORT_SYMBOL(tsc_freq_get);
EXPORT_SYMBOL(tsc_recalibrate_with_freq);
EXPORT_SYMBOL(tsc_delay);
EXPORT_SYMBOL(get_time_ns);
