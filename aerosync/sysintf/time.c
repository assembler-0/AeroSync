/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/time.c
 * @brief Unified Time Subsystem implementation
 * @copyright (C) 2025 assembler-0
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
#include <aerosync/sysintf/time.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <lib/printk.h>
#include <aerosync/fkx/fkx.h>

#define MAX_TIME_SOURCES 8

static const time_source_t *registered_sources[MAX_TIME_SOURCES];
static size_t num_registered_sources = 0;
static const time_source_t *current_time_source = NULL;

void time_register_source(const time_source_t *source) {
  if (num_registered_sources >= MAX_TIME_SOURCES) {
    printk(KERN_WARNING TIME_CLASS
           "Max time sources registered, ignoring '%s'\n",
           source->name);
    return;
  }
  registered_sources[num_registered_sources++] = source;
  printk(KERN_DEBUG TIME_CLASS "Registered time source: %s (prio: %d)\n",
         source->name, source->priority);
}

int time_init(void) {
  const time_source_t *selected = NULL;
  const time_source_t *fallback = NULL;

  printk(TIME_CLASS "Initializing Time Subsystem...\n");

  for (size_t i = 0; i < num_registered_sources; i++) {
    // Simple selection logic: Best priority that initializes successfully
    // We will probe them in order of priority if we sorted them, bu simpler
    // loop is: iterate all, pick best priority.

    // Actually, we should try to init the best priority one.
    // Let's first finding the best candidate.
    if (!selected || registered_sources[i]->priority > selected->priority) {
      fallback = selected;
      selected = registered_sources[i];
    } else if (!fallback ||
               registered_sources[i]->priority > fallback->priority) {
      fallback = registered_sources[i];
    }
  }

  // Attempt to initialize selected
  if (selected) {
    printk(KERN_DEBUG TIME_CLASS "Attempting to initialize best source: %s\n",
           selected->name);
    if (selected->init() != 0) {
      printk(KERN_WARNING TIME_CLASS "Failed to init %s, trying fallback...\n",
             selected->name);
      selected = fallback;
      if (selected && selected->init() != 0) {
        selected = NULL;
      }
    }
  }

  if (!selected) {
    panic(TIME_CLASS "No suitable time source found");
  }

  current_time_source = selected;
  printk(KERN_INFO TIME_CLASS "Selected time source: %s\n",
         current_time_source->name);

  return 0;
}

const char *time_get_source_name(void) {
  if (!current_time_source)
    return "NONE";
  return current_time_source->name;
}

void time_wait_ns(uint64_t ns) {
  if (ns == 0)
    return;

  // Prefer TSC delay if it has been calibrated, as it's low overhead
  if (tsc_freq_get() > 0) {
    tsc_delay(ns);
    return;
  }

  if (!current_time_source) {
    // Very crude fallback early in boot
    volatile uint64_t count = ns / 10;
    while (count--)
      cpu_relax();
    return;
  }

  uint64_t freq = current_time_source->get_frequency();
  uint64_t start_count = current_time_source->read_counter();

  uint64_t ticks_needed = (ns * freq) / 1000000000ULL;
  if (ticks_needed == 0 && ns > 0)
    ticks_needed = 1;

  while (1) {
    uint64_t current_count = current_time_source->read_counter();

    // Standard wrap-around safe difference for up-counters
    if ((current_count - start_count) >= ticks_needed) {
      break;
    }

    cpu_relax();
  }
}

int time_calibrate_tsc_system(void) {
  if (!current_time_source)
    return -1;

  if (current_time_source->calibrate_tsc) {
    return current_time_source->calibrate_tsc();
  }

  // Generic calibration using time_wait_ns (or raw counters)
  printk(KERN_INFO TIME_CLASS
         "Performing generic TSC calibration using %s...\n",
         current_time_source->name);

  uint64_t start_tsc = rdtsc();

  // Wait 50ms (or sufficient time)
  // We can't use time_wait_ns easily if we are defining it.
  // We need to use specific source wait.

  uint64_t freq = current_time_source->get_frequency();
  uint64_t start_counter = current_time_source->read_counter();

  // Wait for approx 50ms
  uint64_t target_ticks = freq / 20; // 50ms

  while (1) {
    uint64_t current_source = current_time_source->read_counter();
    if ((current_source - start_counter) >= target_ticks)
      break; // Assumes simple monotonic up-counter
    cpu_relax();
  }

  uint64_t end_tsc = rdtsc();
  uint64_t tsc_delta = end_tsc - start_tsc;

  // freq = delta * 20
  uint64_t tsc_freq = tsc_delta * 20;

  tsc_recalibrate_with_freq(tsc_freq);
  return 0;
}
EXPORT_SYMBOL(time_register_source);
EXPORT_SYMBOL(time_init);
EXPORT_SYMBOL(time_wait_ns);
EXPORT_SYMBOL(time_calibrate_tsc_system);
