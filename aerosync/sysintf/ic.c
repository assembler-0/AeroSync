/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/ic.c
 * @brief Unified interrupt controller management
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
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/types.h>
#include <aerosync/errno.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>

#include "aerosync/errno.h"

static struct class ic_class = {
    .name = "interrupt_controller",
    .dev_prefix = CONFIG_IC_NAME_PREFIX,
    .naming_scheme = NAMING_NUMERIC,
};

static struct device_driver ic_driver = {
    .name = "ic_core",
};

static bool ic_class_registered = false;
static interrupt_controller_interface_t *current_ops = nullptr;
static uint32_t timer_frequency_hz = IC_DEFAULT_TICK;
static uint8_t (*get_id)(void) = nullptr;

struct ic_device {
  struct device dev;
  const interrupt_controller_interface_t *ops;
};

static void ic_dev_release(struct device *dev) {
  struct ic_device *ic = container_of(dev, struct ic_device, dev);
  if (ic->dev.name && strcmp(ic->dev.name, "ic_device") != 0) {
    kfree((void *)ic->dev.name);
  }
  kfree(ic);
}

void ic_register_controller(
    const interrupt_controller_interface_t *controller) {
  if (unlikely(!ic_class_registered)) {
    class_register(&ic_class);
    ic_class_registered = true;
  }

  struct ic_device *ic = kzalloc(sizeof(struct ic_device));
  if (!ic) {
    panic(IC_CLASS "Failed to allocate IC device");
  }

  ic->ops = controller;
  ic->dev.class = &ic_class;
  ic->dev.driver = &ic_driver;
  ic->dev.release = ic_dev_release;

  if (device_register(&ic->dev) != 0) {
    printk(KERN_ERR IC_CLASS "Failed to register IC device\n");
    kfree(ic);
    return;
  }

  printk(KERN_DEBUG IC_CLASS
         "Registered IC controller type %d (prio: %d) via UDM\n",
         controller->type, controller->priority);
}
EXPORT_SYMBOL(ic_register_controller);

// Helper for iterator
static int __no_cfi ic_find_best(struct device *dev, void *data) {
  struct ic_device *ic = container_of(dev, struct ic_device, dev);
  const interrupt_controller_interface_t **best =
      (const interrupt_controller_interface_t **)data;

  if (ic->ops->probe()) {
    if (!*best || ic->ops->priority > (*best)->priority) {
      *best = ic->ops;
    }
  }
  return 0; // continue calling for all
}

interrupt_controller_t __no_cfi ic_install(int *status) {
  const interrupt_controller_interface_t *selected = nullptr;

  // 1. Find best controller using class iteration
  class_for_each_dev(&ic_class, nullptr, &selected,
    (class_iter_fn)ic_find_best);

  if (!selected) {
    printk(KERN_ERR
        IC_CLASS
        "No interrupt controller could be installed (probe failed for all)\n");
    *status = -ENODEV;
  }

  // 2. Install
  if (!selected->install()) {
    // Simple fallback logic is harder with iterator, for now panic if best
    // fails In a real UDM, we might retry the next best.
    printk(KERN_ERR IC_CLASS "Selected controller type %d install failed\n",
          selected->type);
    *status = -EFAULT;
  }

  printk(KERN_DEBUG IC_CLASS "Configuring timer to %u Hz...\n",
         timer_frequency_hz);
  selected->timer_set(timer_frequency_hz);
  selected->mask_all();
  printk(IC_CLASS "Timer configured.\n");

  // Set current controller ops
  current_ops = (interrupt_controller_interface_t *)selected;

  return current_ops->type;
}

int __no_cfi ic_ap_init(void) {
  if (!current_ops) {
    printk(KERN_ERR IC_CLASS "IC not initialized on BSP before AP init");
    return -ENOSYS;
  }

  if (current_ops->init_ap) {
    if (!current_ops->init_ap()) {
      printk(KERN_ERR IC_CLASS "Failed to initialize interrupt controller on AP");
      return -EFAULT;
    }
  }
  return 0;
}
EXPORT_SYMBOL(ic_ap_init);

void __no_cfi ic_shutdown_controller(void) {
  if (!current_ops)
    return;

  printk(KERN_INFO IC_CLASS "Shutting down interrupt controller...\n");

  // Mask all interrupts first to ensure silence
  if (current_ops->mask_all) {
    current_ops->mask_all();
  }

  // Perform specific shutdown logic if available
  if (current_ops->shutdown) {
    current_ops->shutdown();
  }

  current_ops = nullptr;
}
EXPORT_SYMBOL(ic_shutdown_controller);

void __no_cfi ic_enable_irq(uint32_t irq_line) {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  if (!current_ops->enable_irq)
    panic(IC_CLASS "enable_irq not supported");
  current_ops->enable_irq(irq_line);
}
EXPORT_SYMBOL(ic_enable_irq);

void __no_cfi ic_disable_irq(uint32_t irq_line) {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  if (!current_ops->disable_irq)
    panic(IC_CLASS "disable_irq not supported");
  current_ops->disable_irq(irq_line);
}
EXPORT_SYMBOL(ic_disable_irq);

void __no_cfi ic_send_eoi(uint32_t interrupt_number) {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  if (!current_ops->send_eoi)
    panic(IC_CLASS "send_eoi not supported");
  current_ops->send_eoi(interrupt_number);
}
EXPORT_SYMBOL(ic_send_eoi);

interrupt_controller_t ic_get_controller_type(void) {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  return current_ops->type;
}
EXPORT_SYMBOL(ic_get_controller_type);

int __no_cfi ic_set_timer(const uint32_t frequency_hz) {
  if (!current_ops)
    return -ENOSYS;
  if (current_ops->timer_set) {
    current_ops->timer_set(frequency_hz);
    timer_frequency_hz = frequency_hz;
  }
  return 0;
}
EXPORT_SYMBOL(ic_set_timer);

void __no_cfi ic_timer_stop(void) {
  if (current_ops && current_ops->timer_stop)
    current_ops->timer_stop();
}
EXPORT_SYMBOL(ic_timer_stop);

void __no_cfi ic_timer_oneshot(uint32_t microseconds) {
  if (current_ops && current_ops->timer_oneshot)
    current_ops->timer_oneshot(microseconds);
}
EXPORT_SYMBOL(ic_timer_oneshot);

void __no_cfi ic_timer_tsc_deadline(uint64_t deadline) {
  if (current_ops && current_ops->timer_tsc_deadline)
    current_ops->timer_tsc_deadline(deadline);
}
EXPORT_SYMBOL(ic_timer_tsc_deadline);

int __no_cfi ic_timer_has_tsc_deadline(void) {
  if (current_ops && current_ops->timer_has_tsc_deadline)
    return current_ops->timer_has_tsc_deadline();
  return 0;
}
EXPORT_SYMBOL(ic_timer_has_tsc_deadline);

void __no_cfi ic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  if (current_ops->type != INTC_APIC || !current_ops->send_ipi) {
    panic(IC_CLASS "IPIs only supported on APIC controllers");
  }
  current_ops->send_ipi(dest_apic_id, vector, delivery_mode);
}
EXPORT_SYMBOL(ic_send_ipi);

static uint8_t ic_get_id_non_smp(void) { return 0; };

static int __no_cfi ic_find_get_id(struct device *dev, void *data) {
  struct ic_device *ic = container_of(dev, struct ic_device, dev);
  if (ic->ops->type == INTC_APIC && ic->ops->get_id) {
    get_id = ic->ops->get_id;
    return 1; // stop
  }
  return 0;
}

int ic_register_lapic_get_id_early() {
  class_for_each_dev(&ic_class, nullptr, nullptr,
    (class_iter_fn)ic_find_get_id);

  if (!get_id) {
    get_id = ic_get_id_non_smp;
  }
  return 0;
}

uint8_t __no_cfi ic_lapic_get_id(void) {
  if (!get_id) {
    panic(IC_CLASS "IC not initialized");
  }
  return get_id();
}

void __no_cfi ic_mask_all() {
  if (!current_ops)
    panic(IC_CLASS "IC not initialized");
  if (!current_ops->mask_all)
    panic(IC_CLASS "mask_all not supported");
  current_ops->mask_all();
}
EXPORT_SYMBOL(ic_mask_all);

uint32_t ic_get_frequency(void) { return timer_frequency_hz; }
EXPORT_SYMBOL(ic_get_frequency);
