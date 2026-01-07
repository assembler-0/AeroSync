/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/pci/pci.c
 * @brief Modern PCI Subsystem Implementation (Registered Subsystem)
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/classes.h>
#include <kernel/sysintf/pci.h>
#include <kernel/fkx/fkx.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <linux/container_of.h>
#include <drivers/pci/backend_ecam.h>
#include <drivers/pci/backend_pio.h>

static LIST_HEAD(pci_devices);
static LIST_HEAD(pci_drivers);
static LIST_HEAD(pci_root_buses);

static const struct pci_device_id *pci_match_one_device(const struct pci_device_id *id, struct pci_dev *dev) {
  if ((id->vendor == PCI_ANY_ID || id->vendor == dev->vendor) &&
      (id->device == PCI_ANY_ID || id->device == dev->device) &&
      (id->subvendor == PCI_ANY_ID || id->subvendor == dev->subsystem_vendor) &&
      (id->subdevice == PCI_ANY_ID || id->subdevice == dev->subsystem_device) &&
      !((id->class ^ dev->class) & id->class_mask))
    return id;
  return NULL;
}

static const struct pci_device_id *pci_match_device(struct pci_driver *drv, struct pci_dev *dev) {
  const struct pci_device_id *ids = drv->id_table;
  if (ids) {
    while (ids->vendor || ids->subvendor || ids->class_mask) {
      if (pci_match_one_device(ids, dev))
        return ids;
      ids++;
    }
  }
  return NULL;
}

static int pci_bus_match(struct pci_dev *dev, struct pci_driver *drv) {
  const struct pci_device_id *id;

  id = pci_match_device(drv, dev);
  if (id) {
    if (drv->probe(dev, id) >= 0) {
      dev->driver = drv;
      return 1;
    }
  }
  return 0;
}

static int subsys_register_driver(struct pci_driver *driver) {
  list_add_tail(&driver->node, &pci_drivers);

  struct pci_dev *dev;
  list_for_each_entry(dev, &pci_devices, global_list) {
    if (!dev->driver) {
      pci_bus_match(dev, driver);
    }
  }

  return 0;
}

static void subsys_unregister_driver(struct pci_driver *driver) {
  struct pci_dev *dev;
  list_for_each_entry(dev, &pci_devices, global_list) {
    if (dev->driver == driver) {
      if (driver->remove)
        driver->remove(dev);
      dev->driver = NULL;
    }
  }
  list_del(&driver->node);
}

static void pci_scan_device(struct pci_bus *bus, uint8_t devfn) {
  pci_handle_t handle = {bus->segment, bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn)};
  uint32_t id = pci_read(&handle, PCI_VENDOR_ID, 32);

  if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000)
    return;

  struct pci_dev *dev = kmalloc(sizeof(struct pci_dev));
  if (!dev) return;

  memset(dev, 0, sizeof(*dev));
  dev->bus = bus;
  dev->devfn = devfn;
  dev->handle = handle;
  dev->vendor = id & 0xFFFF;
  dev->device = id >> 16;

  uint32_t class_rev = pci_read(&handle, PCI_REVISION_ID, 32);
  dev->revision = class_rev & 0xFF;
  dev->class = class_rev >> 8;

  dev->hdr_type = pci_read(&handle, PCI_HEADER_TYPE, 8);

  // Read BARs for standard devices
  if ((dev->hdr_type & 0x7F) == 0) {
    for (int i = 0; i < 6; i++) {
      uint32_t bar_off = PCI_BAR0 + (i * 4);
      uint32_t bar_val = pci_read(&handle, bar_off, 32);
      dev->bars[i] = bar_val;

      // Determine BAR size
      pci_write(&handle, bar_off, 0xFFFFFFFF, 32);
      uint32_t size_val = pci_read(&handle, bar_off, 32);
      pci_write(&handle, bar_off, bar_val, 32);

      if (size_val != 0 && size_val != 0xFFFFFFFF) {
        if (bar_val & 1) {
          // I/O
          dev->bar_sizes[i] = ~(size_val & 0xFFFFFFFC) + 1;
        } else {
          // Memory
          dev->bar_sizes[i] = ~(size_val & 0xFFFFFFF0) + 1;
        }
      }
    }
  }

  list_add_tail(&dev->bus_list, &bus->devices);
  list_add_tail(&dev->global_list, &pci_devices);

  printk(KERN_DEBUG PCI_CLASS "Found device %02x:%02x.%d [%04x:%04x] class %06x\n",
         bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), dev->vendor, dev->device, dev->class);

  // Check for drivers
  struct pci_driver *drv;
  list_for_each_entry(drv, &pci_drivers, node) {
    if (pci_bus_match(dev, drv))
      break;
  }

  // If it's a bridge, scan secondary bus
  if ((dev->class >> 8) == 0x0604) {
    uint8_t secondary_bus_num = pci_read(&handle, 0x19, 8);
    struct pci_bus *child_bus = kmalloc(sizeof(struct pci_bus));
    if (child_bus) {
      memset(child_bus, 0, sizeof(*child_bus));
      child_bus->number = secondary_bus_num;
      child_bus->segment = bus->segment;
      child_bus->parent = bus;
      INIT_LIST_HEAD(&child_bus->devices);
      INIT_LIST_HEAD(&child_bus->children);
      list_add_tail(&child_bus->node, &bus->children);
      pci_enumerate_bus(child_bus);
    }
  }
}

static void subsys_enumerate_bus(struct pci_bus *bus) {
  for (uint8_t slot = 0; slot < 32; slot++) {
    pci_handle_t handle = {bus->segment, bus->number, slot, 0};
    uint16_t vendor = pci_read(&handle, PCI_VENDOR_ID, 16);
    if (vendor == 0xFFFF) continue;

    uint8_t hdr_type = pci_read(&handle, PCI_HEADER_TYPE, 8);
    uint8_t func_count = (hdr_type & 0x80) ? 8 : 1;

    for (uint8_t func = 0; func < func_count; func++) {
      pci_scan_device(bus, PCI_DEVFN(slot, func));
    }
  }
}

static int subsys_enable_device(struct pci_dev *dev) {
  uint16_t cmd = pci_read_config16(dev, PCI_COMMAND);
  cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
  pci_write_config16(dev, PCI_COMMAND, cmd);
  return 0;
}

static void subsys_set_master(struct pci_dev *dev) {
  uint16_t cmd = pci_read_config16(dev, PCI_COMMAND);
  cmd |= PCI_COMMAND_MASTER;
  pci_write_config16(dev, PCI_COMMAND, cmd);
}

static pci_subsystem_ops_t subsys_ops = {
  .register_driver = subsys_register_driver,
  .unregister_driver = subsys_unregister_driver,
  .enumerate_bus = subsys_enumerate_bus,
  .enable_device = subsys_enable_device,
  .set_master = subsys_set_master,
};

static int pci_mod_init(void) {
  printk(KERN_INFO PCI_CLASS "Initializing PCI Subsystem\n");

  // 1. Register Subsystem Interface
  pci_register_subsystem(&subsys_ops);

  // 2. Initialize Backends
  pci_backend_pio_init();
  pci_backend_ecam_init();

  // 3. Create root bus 0 and scan
  struct pci_bus *root_bus = kmalloc(sizeof(struct pci_bus));
  if (!root_bus) return -1;

  memset(root_bus, 0, sizeof(*root_bus));
  root_bus->number = 0;
  root_bus->segment = 0;
  INIT_LIST_HEAD(&root_bus->devices);
  INIT_LIST_HEAD(&root_bus->children);
  list_add_tail(&root_bus->node, &pci_root_buses);

  pci_enumerate_bus(root_bus);

  return 0;
}

FKX_MODULE_DEFINE(
  pci,
  "1.0.0",
  "assembler-0",
  "Modern PCI Subsystem Core",
  0,
  FKX_DRIVER_CLASS,
  pci_mod_init,
  NULL
);
