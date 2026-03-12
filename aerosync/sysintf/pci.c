/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/pci.c
 * @brief PCI System Interface Implementation (Dynamic Version)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/pci.h>
#include <aerosync/rcu.h>
#include <lib/printk.h>
#include <lib/string.h>

#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <mm/slub.h>
#include <linux/container_of.h>

static int pci_hw_evaluate(struct device *a, struct device *b) {
  const pci_ops_t *ops_a = a->ops;
  const pci_ops_t *ops_b = b->ops;
  
  if (ops_a->probe && ops_a->probe() != 0) return -1;
  return (int)(ops_a->priority - ops_b->priority);
}

static struct class pci_hw_class = {
  .name = "pci_hardware",
  .is_singleton = true,
  .evaluate = pci_hw_evaluate,
};

static bool pci_hw_class_registered = false;
static const pci_subsystem_ops_t *current_subsys_ops = nullptr;

struct pci_hw_device {
  struct device dev;
};

static void pci_hw_release(struct device *dev) {
  struct pci_hw_device *phw = container_of(dev, struct pci_hw_device, dev);
  kfree(phw);
}

void __no_cfi pci_register_ops(const pci_ops_t *ops) {
  if (unlikely(!pci_hw_class_registered)) {
    class_register(&pci_hw_class);
    pci_hw_class_registered = true;
  }

  struct pci_hw_device *phw = kzalloc(sizeof(struct pci_hw_device));
  if (!phw) return;

  phw->dev.ops = (void *)ops;
  phw->dev.class = &pci_hw_class;
  phw->dev.name = ops->name;
  phw->dev.release = pci_hw_release;

  if (device_register(&phw->dev) != 0) {
    kfree(phw);
    return;
  }

  printk(KERN_DEBUG PCI_CLASS "Registered PCI hardware ops: %s\n", ops->name);
}
EXPORT_SYMBOL(pci_register_ops);

void pci_register_subsystem(const pci_subsystem_ops_t *ops) {
  /* For now, subsystem ops are still a simple pointer as they don't have multiple providers usually */
  current_subsys_ops = ops;
  printk(KERN_INFO PCI_CLASS "PCI Subsystem core registered\n");
}
EXPORT_SYMBOL(pci_register_subsystem);

/* Low-level Config Access Dispatchers */

uint32_t __no_cfi pci_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  uint32_t val = 0xFFFFFFFF;
  rcu_read_lock();
  const pci_ops_t *ops = class_get_active_interface(&pci_hw_class);
  if (ops) val = ops->read(p, offset, width);
  rcu_read_unlock();
  return val;
}
EXPORT_SYMBOL(pci_read);

void __no_cfi pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
  rcu_read_lock();
  const pci_ops_t *ops = class_get_active_interface(&pci_hw_class);
  if (ops) ops->write(p, offset, val, width);
  rcu_read_unlock();
}
EXPORT_SYMBOL(pci_write);

/* High-level Subsystem Dispatchers */

#define PCI_SUBSYS_CALL(func, ...) ({ \
  int ret = -ENODEV; \
  if (current_subsys_ops && current_subsys_ops->func) \
    ret = current_subsys_ops->func(__VA_ARGS__); \
  ret; \
})

int __no_cfi pci_register_driver(struct pci_driver *driver) {
  return PCI_SUBSYS_CALL(register_driver, driver);
}
EXPORT_SYMBOL(pci_register_driver);

void __no_cfi pci_unregister_driver(struct pci_driver *driver) {
  if (current_subsys_ops && current_subsys_ops->unregister_driver)
    current_subsys_ops->unregister_driver(driver);
}
EXPORT_SYMBOL(pci_unregister_driver);

void __no_cfi pci_enumerate_bus(struct pci_bus *bus) {
  if (current_subsys_ops && current_subsys_ops->enumerate_bus)
    current_subsys_ops->enumerate_bus(bus);
}
EXPORT_SYMBOL(pci_enumerate_bus);

int __no_cfi pci_enable_device(struct pci_dev *dev) {
  return PCI_SUBSYS_CALL(enable_device, dev);
}
EXPORT_SYMBOL(pci_enable_device);

void __no_cfi pci_set_master(struct pci_dev *dev) {
  if (current_subsys_ops && current_subsys_ops->set_master)
    current_subsys_ops->set_master(dev);
}
EXPORT_SYMBOL(pci_set_master);
