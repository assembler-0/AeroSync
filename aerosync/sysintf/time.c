/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/time.c
 * @brief Unified Time Subsystem implementation
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

#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/time.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/tsc.h>
#include <lib/printk.h>

#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <lib/string.h>
#include <lib/vsprintf.h>
#include <mm/slub.h>

static struct class time_class = {
    .name = "time_source",
};

static bool time_class_registered = false;
static const time_source_t *current_time_source = NULL;

struct time_device {
  struct device dev;
  const time_source_t *source;
};

static void time_dev_release(struct device *dev) {
  struct time_device *td = container_of(dev, struct time_device, dev);
  if (td->dev.name && strcmp(td->dev.name, "time_device") != 0) {
    kfree((void *)td->dev.name);
  }
  kfree(td);
}

void time_register_source(const time_source_t *source) {
  if (unlikely(!time_class_registered)) {
    class_register(&time_class);
    time_class_registered = true;
  }

  struct time_device *td = kzalloc(sizeof(struct time_device));
  if (!td) {
    panic(TIME_CLASS "Failed to allocate time device");
  }

  td->source = source;
  td->dev.class = &time_class;
  td->dev.release = time_dev_release;

  // Name: time_[name]
  char *name_buf = kzalloc(32);
  if (name_buf) {
    snprintf(name_buf, 32, "time_%s", source->name);
    td->dev.name = name_buf;
  } else {
    td->dev.name = "time_device";
  }

  if (device_register(&td->dev) != 0) {
    printk(KERN_ERR TIME_CLASS "Failed to register time device\n");
    if (name_buf)
      kfree(name_buf);
    kfree(td);
    return;
  }

  printk(KERN_DEBUG TIME_CLASS
         "Registered time source: %s (prio: %d) via UDM\n",
         source->name, source->priority);
}

#define MAX_CANDIDATES 16

       struct time_candidate_list {
         const time_source_t *candidates[MAX_CANDIDATES];
         int count;
       };

       static int time_collect_candidates(struct device *dev, void *data) {
         struct time_candidate_list *list = (struct time_candidate_list *)data;
         struct time_device *td = container_of(dev, struct time_device, dev);

         if (list->count < MAX_CANDIDATES) {
           list->candidates[list->count++] = td->source;
         }
         return 0;
       }

       int time_init(void) {
         const time_source_t *selected = NULL;
         struct time_candidate_list list = {0};

         printk(TIME_CLASS "Initializing Time Subsystem...\n");

         // 1. Collect all registered sources through UDM
         class_for_each_dev(&time_class, NULL, &list, time_collect_candidates);

         // 2. Fallback logic: Try initialization in order of priority
         // (simplistic bubble sort) Since count is small (usually 2-3: TSC,
         // HPET, PIT), this is fine.
         for (int i = 0; i < list.count - 1; i++) {
           for (int j = 0; j < list.count - i - 1; j++) {
             if (list.candidates[j]->priority <
                 list.candidates[j + 1]->priority) {
               const time_source_t *temp = list.candidates[j];
               list.candidates[j] = list.candidates[j + 1];
               list.candidates[j + 1] = temp;
             }
           }
         }

         // 3. Try to init best available
         for (int i = 0; i < list.count; i++) {
           const time_source_t *candidate = list.candidates[i];
           printk(KERN_DEBUG TIME_CLASS
                  "Attempting to initialize source: %s (prio %d)\n",
                  candidate->name, candidate->priority);

           if (candidate->init() == 0) {
             selected = candidate;
             break;
           } else {
             printk(KERN_WARNING TIME_CLASS "Failed to init %s\n",
                    candidate->name);
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
