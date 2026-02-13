/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/pci.c
 * @brief PCI System Interface Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/pci.h>
#include <lib/printk.h>
#include <lib/string.h>

#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <mm/slub.h>
#include <linux/container_of.h>
#include <aerosync/errno.h>

static struct class pci_hw_class = {
    .name = "pci_hardware",
};

static bool pci_hw_class_registered = false;
static const pci_ops_t *current_hw_ops = nullptr;
static const pci_subsystem_ops_t *current_subsys_ops = nullptr;

struct pci_hw_device {
  struct device dev;
  const pci_ops_t *ops;
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
  if (!phw)
    return;

  phw->ops = ops;
  phw->dev.class = &pci_hw_class;
  phw->dev.name = ops->name;
  phw->dev.release = pci_hw_release;

  if (device_register(&phw->dev) != 0) {
    kfree(phw);
    return;
  }

  /* Automatically switch if this is higher priority and probes OK */
  if (!current_hw_ops || ops->priority > current_hw_ops->priority) {
    if (ops->probe && ops->probe() == 0) {
      current_hw_ops = ops;
      printk(KERN_DEBUG PCI_CLASS
             "Selected %s for PCI hardware access (prio %d)\n",
             ops->name, ops->priority);
    }
  }
}

EXPORT_SYMBOL(pci_register_ops);

void pci_register_subsystem(const pci_subsystem_ops_t *ops) {
  current_subsys_ops = ops;
  printk(KERN_INFO PCI_CLASS "PCI Subsystem core registered\n");
}

EXPORT_SYMBOL(pci_register_subsystem);

/* Low-level Config Access Dispatchers */

uint32_t __no_cfi pci_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  if (current_hw_ops)
    return current_hw_ops->read(p, offset, width);
  return 0xFFFFFFFF;
}

EXPORT_SYMBOL(pci_read);

void __no_cfi pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
  if (current_hw_ops)
    current_hw_ops->write(p, offset, val, width);
}

EXPORT_SYMBOL(pci_write);

/* High-level Subsystem Dispatchers */

int __no_cfi pci_register_driver(struct pci_driver *driver) {
  if (current_subsys_ops && current_subsys_ops->register_driver)
    return current_subsys_ops->register_driver(driver);
  return -ENODEV;
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
  if (current_subsys_ops && current_subsys_ops->enable_device)
    return current_subsys_ops->enable_device(dev);
  return -ENODEV;
}

EXPORT_SYMBOL(pci_enable_device);

void __no_cfi pci_set_master(struct pci_dev *dev) {
  if (current_subsys_ops && current_subsys_ops->set_master)
    current_subsys_ops->set_master(dev);
}

EXPORT_SYMBOL(pci_set_master);
