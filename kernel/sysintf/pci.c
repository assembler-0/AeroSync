///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sysintf/pci.c
 * @brief PCI System Interface Implementation
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/sysintf/pci.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/classes.h>
#include <kernel/fkx/fkx.h>

#define MAX_PCI_OPS 4
static const pci_ops_t *registered_ops[MAX_PCI_OPS];
static int num_registered_ops = 0;
static const pci_ops_t *current_hw_ops = NULL;

static const pci_subsystem_ops_t *current_subsys_ops = NULL;

void pci_register_ops(const pci_ops_t *ops) {
  if (num_registered_ops >= MAX_PCI_OPS) return;
  registered_ops[num_registered_ops++] = ops;

  if (!current_hw_ops || ops->priority > current_hw_ops->priority) {
    if (ops->probe && ops->probe() == 0) {
      current_hw_ops = ops;
      printk(KERN_DEBUG PCI_CLASS "Switched to %s hardware ops\n", ops->name);
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

uint32_t pci_read(pci_handle_t *p, uint32_t offset, uint8_t width) {
  if (current_hw_ops) return current_hw_ops->read(p, offset, width);
  return 0xFFFFFFFF;
}

EXPORT_SYMBOL(pci_read);

void pci_write(pci_handle_t *p, uint32_t offset, uint32_t val, uint8_t width) {
  if (current_hw_ops) current_hw_ops->write(p, offset, val, width);
}

EXPORT_SYMBOL(pci_write);

/* High-level Subsystem Dispatchers */

int pci_register_driver(struct pci_driver *driver) {
  if (current_subsys_ops && current_subsys_ops->register_driver)
    return current_subsys_ops->register_driver(driver);
  return -1;
}

EXPORT_SYMBOL(pci_register_driver);

void pci_unregister_driver(struct pci_driver *driver) {
  if (current_subsys_ops && current_subsys_ops->unregister_driver)
    current_subsys_ops->unregister_driver(driver);
}

EXPORT_SYMBOL(pci_unregister_driver);

void pci_enumerate_bus(struct pci_bus *bus) {
  if (current_subsys_ops && current_subsys_ops->enumerate_bus)
    current_subsys_ops->enumerate_bus(bus);
}

EXPORT_SYMBOL(pci_enumerate_bus);

int pci_enable_device(struct pci_dev *dev) {
  if (current_subsys_ops && current_subsys_ops->enable_device)
    return current_subsys_ops->enable_device(dev);
  return -1;
}

EXPORT_SYMBOL(pci_enable_device);

void pci_set_master(struct pci_dev *dev) {
  if (current_subsys_ops && current_subsys_ops->set_master)
    current_subsys_ops->set_master(dev);
}

EXPORT_SYMBOL(pci_set_master);
