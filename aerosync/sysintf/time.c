/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/time.c
 * @brief Unified Time Subsystem (Unified Model)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/time.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/tsc.h>
#include <lib/printk.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/rcu.h>
#include <lib/string.h>
#include <mm/slub.h>

static int time_evaluate(struct device *a, struct device *b) {
  const time_source_t *ops_a = a->ops;
  const time_source_t *ops_b = b->ops;
  return (int)(ops_a->priority - ops_b->priority);
}

static struct class time_class = {
  .name = "time_source",
  .dev_prefix = CONFIG_TIME_NAME_PREFIX,
  .naming_scheme = NAMING_NUMERIC,
  .is_singleton = true,
  .evaluate = time_evaluate,
};

static void time_dev_shutdown(struct device *dev) {
  const time_source_t *ops = dev->ops;
  if (ops && ops->stop) ops->stop();
}

static int time_dev_suspend(struct device *dev) {
  const time_source_t *ops = dev->ops;
  if (ops && ops->stop) ops->stop();
  return 0;
}

static int time_dev_resume(struct device *dev) {
  const time_source_t *ops = dev->ops;
  if (ops && ops->init) ops->init();
  return 0;
}

static struct device_driver time_driver = {
  .name = "time_core",
  .shutdown = time_dev_shutdown,
  .suspend = time_dev_suspend,
  .resume = time_dev_resume,
};

static bool time_class_registered = false;

struct time_device {
  struct device dev;
};

static void time_dev_release(struct device *dev) {
  struct time_device *td = container_of(dev, struct time_device, dev);
  kfree(td);
}

int time_register_source(const time_source_t *source) {
  if (unlikely(!time_class_registered)) {
    class_register(&time_class);
    time_class_registered = true;
  }

  struct time_device *td = kzalloc(sizeof(struct time_device));
  if (!td) return -ENOMEM;

  td->dev.ops = (void *)source;
  td->dev.class = &time_class;
  td->dev.driver = &time_driver;
  td->dev.release = time_dev_release;

  if (device_register(&td->dev) != 0) {
    printk(KERN_ERR TIME_CLASS "Failed to register time device\n");
    kfree(td);
    return -EFAULT;
  }

  return 0;
}
EXPORT_SYMBOL(time_register_source);

int time_unregister_source(const time_source_t *source) {
  struct device *dev;
  mutex_lock(&time_class.lock);
  list_for_each_entry(dev, &time_class.devices, class_node) {
    if (dev->ops == source) {
      mutex_unlock(&time_class.lock);
      device_unregister(dev);
      return 0;
    }
  }
  mutex_unlock(&time_class.lock);
  return -ENODEV;
}
EXPORT_SYMBOL(time_unregister_source);

#define TIME_SOURCE() ((time_source_t *)class_get_active_interface(&time_class))

int time_init(void) {
  printk(TIME_CLASS "Initializing Time Subsystem...\n");
  /* The driver model handles selection automatically */
  time_source_t *source = TIME_SOURCE();
  if (source && source->init) return source->init();
  return -ENODEV;
}
EXPORT_SYMBOL(time_init);

const char *time_get_source_name(void) {
  time_source_t *source = TIME_SOURCE();
  return source ? source->name : "NONE";
}

void __no_cfi time_wait_ns(uint64_t ns) {
  if (ns == 0) return;

  if (tsc_freq_get() > 0) {
    tsc_delay(ns);
    return;
  }

  time_source_t *source = TIME_SOURCE();
  if (!source) {
    volatile uint64_t count = ns / 10;
    while (count--) cpu_relax();
    return;
  }

  uint64_t freq = source->get_frequency();
  uint64_t start_count = source->read_counter();
  uint64_t ticks_needed = (ns * freq) / 1000000000ULL;
  if (ticks_needed == 0 && ns > 0) ticks_needed = 1;

  while (1) {
    uint64_t current_count = source->read_counter();
    if ((current_count - start_count) >= ticks_needed) break;
    cpu_relax();
  }
}
EXPORT_SYMBOL(time_wait_ns);

int __no_cfi time_calibrate_tsc_system(void) {
  time_source_t *source = TIME_SOURCE();
  if (!source) return -ENODEV;

  if (source->calibrate_tsc) {
    return source->calibrate_tsc();
  }

  printk(KERN_INFO TIME_CLASS
         "Performing generic TSC calibration using %s...\n",
         source->name);

  uint64_t start_tsc = rdtsc();
  uint64_t freq = source->get_frequency();
  uint64_t start_counter = source->read_counter();
  uint64_t target_ticks = freq / 20; // 50ms

  while (1) {
    uint64_t current_source = source->read_counter();
    if ((current_source - start_counter) >= target_ticks) break;
    cpu_relax();
  }

  uint64_t end_tsc = rdtsc();
  uint64_t tsc_delta = end_tsc - start_tsc;
  uint64_t tsc_freq = tsc_delta * 20;

  tsc_recalibrate_with_freq(tsc_freq);
  return 0;
}
EXPORT_SYMBOL(time_calibrate_tsc_system);
