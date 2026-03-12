/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/ic.c
 * @brief Unified interrupt controller management (Unified Model)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/ic.h>
#include <aerosync/types.h>
#include <aerosync/errno.h>
#include <aerosync/rcu.h>
#include <lib/printk.h>
#include <mm/slub.h>

static int ic_evaluate(struct device *a, struct device *b) {
  const interrupt_controller_interface_t *ops_a = a->ops;
  const interrupt_controller_interface_t *ops_b = b->ops;
  if (!ops_a->probe()) return -1;
  return (int)(ops_a->priority - ops_b->priority);
}

static struct class ic_class = {
  .name = "interrupt_controller",
  .dev_prefix = CONFIG_IC_NAME_PREFIX,
  .naming_scheme = NAMING_NUMERIC,
  .is_singleton = true,
  .evaluate = ic_evaluate,
};

 static void ic_dev_shutdown(struct device *dev) {
  const interrupt_controller_interface_t *ops = dev->ops;
  if (ops && ops->shutdown) ops->shutdown();
}

static int ic_dev_suspend(struct device *dev) {
  const interrupt_controller_interface_t *ops = dev->ops;
  if (ops && ops->mask_all) ops->mask_all();
  return 0;
}

static int ic_dev_resume(struct device *dev) {
  const interrupt_controller_interface_t *ops = dev->ops;
  /* Re-install/re-init on resume */
  if (ops && ops->install) ops->install();
  return 0;
}

static struct device_driver ic_driver = {
  .name = "ic_core",
  .shutdown = ic_dev_shutdown,
  .suspend = ic_dev_suspend,
  .resume = ic_dev_resume,
};

static bool ic_class_registered = false;
static uint32_t timer_frequency_hz = IC_DEFAULT_TICK;

struct ic_device {
  struct device dev;
};

static void ic_dev_release(struct device *dev) {
  struct ic_device *ic = container_of(dev, struct ic_device, dev);
  kfree(ic);
}

void ic_register_controller(const interrupt_controller_interface_t *controller) {
  if (unlikely(!ic_class_registered)) {
    class_register(&ic_class);
    ic_class_registered = true;
  }

  struct ic_device *ic = kzalloc(sizeof(struct ic_device));
  if (!ic) panic(IC_CLASS "Failed to allocate IC device");

  ic->dev.ops = (void *)controller;
  ic->dev.class = &ic_class;
  ic->dev.driver = &ic_driver;
  ic->dev.release = ic_dev_release;

  if (device_register(&ic->dev) != 0) {
    printk(KERN_ERR IC_CLASS "Failed to register IC device\n");
    kfree(ic);
    return;
  }
}
EXPORT_SYMBOL(ic_register_controller);

void ic_unregister_controller(const interrupt_controller_interface_t *controller) {
  struct device *dev;
  mutex_lock(&ic_class.lock);
  list_for_each_entry(dev, &ic_class.devices, class_node) {
    if (dev->ops == controller) {
      mutex_unlock(&ic_class.lock);
      device_unregister(dev);
      return;
    }
  }
  mutex_unlock(&ic_class.lock);
}
EXPORT_SYMBOL(ic_unregister_controller);

#define IC_OPS() ((interrupt_controller_interface_t *)class_get_active_interface(&ic_class))

interrupt_controller_t ic_install(int *status) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (!ops) {
    *status = -ENODEV;
    return INTC_UNKNOWN;
  }

  if (ops->install && !ops->install()) {
    *status = -EFAULT;
    return INTC_UNKNOWN;
  }

  if (ops->timer_set) ops->timer_set(timer_frequency_hz);
  if (ops->mask_all) ops->mask_all();

  *status = 0;
  return ops->type;
}

int ic_ap_init(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->init_ap) {
    return ops->init_ap() ? 0 : -EFAULT;
  }
  return -ENOSYS;
}
EXPORT_SYMBOL(ic_ap_init);

void ic_shutdown_controller(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops) {
    if (ops->mask_all) ops->mask_all();
    if (ops->shutdown) ops->shutdown();
  }
}
EXPORT_SYMBOL(ic_shutdown_controller);

/* --- Accessors --- */

void ic_enable_irq(uint32_t irq_line) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->enable_irq) ops->enable_irq(irq_line);
}
EXPORT_SYMBOL(ic_enable_irq);

void ic_disable_irq(uint32_t irq_line) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->disable_irq) ops->disable_irq(irq_line);
}
EXPORT_SYMBOL(ic_disable_irq);

void ic_send_eoi(uint32_t interrupt_number) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->send_eoi) ops->send_eoi(interrupt_number);
}
EXPORT_SYMBOL(ic_send_eoi);

void ic_mask_all(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->mask_all) ops->mask_all();
}
EXPORT_SYMBOL(ic_mask_all);

interrupt_controller_t ic_get_controller_type(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  return ops ? ops->type : INTC_UNKNOWN;
}
EXPORT_SYMBOL(ic_get_controller_type);

int ic_set_timer(const uint32_t frequency_hz) {
  timer_frequency_hz = frequency_hz;
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->timer_set) ops->timer_set(frequency_hz);
  return 0;
}
EXPORT_SYMBOL(ic_set_timer);

void ic_timer_stop(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->timer_stop) ops->timer_stop();
}
EXPORT_SYMBOL(ic_timer_stop);

void ic_timer_oneshot(uint32_t microseconds) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->timer_oneshot) ops->timer_oneshot(microseconds);
}
EXPORT_SYMBOL(ic_timer_oneshot);

void ic_timer_tsc_deadline(uint64_t deadline) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->timer_tsc_deadline) ops->timer_tsc_deadline(deadline);
}
EXPORT_SYMBOL(ic_timer_tsc_deadline);

int ic_timer_has_tsc_deadline(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  return (ops && ops->timer_has_tsc_deadline) ? ops->timer_has_tsc_deadline() : 0;
}
EXPORT_SYMBOL(ic_timer_has_tsc_deadline);

void ic_send_ipi(uint8_t dest_apic_id, uint8_t vector, uint32_t delivery_mode) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->type == INTC_APIC && ops->send_ipi) {
    ops->send_ipi(dest_apic_id, vector, delivery_mode);
  }
}
EXPORT_SYMBOL(ic_send_ipi);

uint8_t ic_lapic_get_id(void) {
  interrupt_controller_interface_t *ops = IC_OPS();
  if (ops && ops->type == INTC_APIC && ops->get_id) return ops->get_id();
  return 0;
}
EXPORT_SYMBOL(ic_lapic_get_id);

int ic_register_lapic_get_id_early() { return 0; }
uint32_t ic_get_frequency(void) { return timer_frequency_hz; }
EXPORT_SYMBOL(ic_get_frequency);
