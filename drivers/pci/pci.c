/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/pci/pci.c
 * @brief Modern PCI Subsystem Implementation (Registered Subsystem)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sysintf/pci.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/errno.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>
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
  return nullptr;
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
  return nullptr;
}

static int __no_cfi pci_bus_match(struct device *dev, struct device_driver *drv) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  struct pci_driver *pci_drv = to_pci_driver(drv);
  const struct pci_device_id *id;

  id = pci_match_device(pci_drv, pci_dev);
  if (id)
    return 1;
  return 0;
}

static int __no_cfi pci_device_probe(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  struct pci_driver *pci_drv = to_pci_driver(dev->driver);
  const struct pci_device_id *id;

  id = pci_match_device(pci_drv, pci_dev);
  if (id) {
    return pci_drv->probe(pci_dev, id);
  }
  return -ENODEV;
}

static void __no_cfi pci_device_remove(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  struct pci_driver *pci_drv = to_pci_driver(dev->driver);

  if (pci_drv->remove)
    pci_drv->remove(pci_dev);
}

static void pci_device_shutdown(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  uint16_t cmd = pci_read_config16(pci_dev, PCI_COMMAND);
  cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
  pci_write_config16(pci_dev, PCI_COMMAND, cmd);
}

static int pci_device_suspend(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  uint16_t cmd = pci_read_config16(pci_dev, PCI_COMMAND);
  cmd &= ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
  pci_write_config16(pci_dev, PCI_COMMAND, cmd);
  return 0;
}

static int pci_device_resume(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  uint16_t cmd = pci_read_config16(pci_dev, PCI_COMMAND);
  cmd |= (PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
  pci_write_config16(pci_dev, PCI_COMMAND, cmd);
  return 0;
}

struct bus_type pci_bus_type = {
  .name = "pci",
  .match = pci_bus_match,
  .probe = pci_device_probe,
  .remove = pci_device_remove,
};

static int subsys_register_driver(struct pci_driver *driver) {
  driver->driver.bus = &pci_bus_type;
  return driver_register(&driver->driver);
}

static void subsys_unregister_driver(struct pci_driver *driver) {
  driver_unregister(&driver->driver);
}

static void pci_dev_release(struct device *dev) {
  struct pci_dev *pci_dev = to_pci_dev(dev);
  kfree(pci_dev);
}

static void pci_scan_device(struct pci_bus *bus, uint8_t devfn) {
  pci_handle_t handle = {bus->segment, bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn)};
  uint32_t id = pci_read(&handle, PCI_VENDOR_ID, 32);

  if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0x0000)
    return;

  struct pci_dev *dev = kzalloc(sizeof(struct pci_dev));
  if (!dev) return;

  dev->pbus = bus;
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
          dev->bar_sizes[i] = ~(size_val & 0xFFFFFFFC) + 1;
        } else {
          dev->bar_sizes[i] = ~(size_val & 0xFFFFFFF0) + 1;
        }
      }
    }
  }

  device_initialize(&dev->dev);
  INIT_LIST_HEAD(&dev->bus_list);
  dev->dev.bus = &pci_bus_type;
  dev->dev.release = pci_dev_release;
  
  const char *pci_prefix = STRINGIFY(CONFIG_PCI_NAME_PREFIX);
  if (pci_prefix[0] != '\0') {
    device_set_name(&dev->dev, "%s_%04x:%02x:%02x.%d", pci_prefix, bus->segment, bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn));
  } else {
    device_set_name(&dev->dev, "%04x:%02x:%02x.%d", bus->segment, bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn));
  }

  if (device_add(&dev->dev) != 0) {
    kfree(dev);
    return;
  }

  list_add_tail(&dev->bus_list, &bus->devices);

  printk(KERN_DEBUG PCI_CLASS "Found device %s [%04x:%04x] class %06x\n",
         dev->dev.name, dev->vendor, dev->device, dev->class);

  // If it's a bridge, scan secondary bus
  if ((dev->class >> 8) == 0x0604) {
    uint8_t secondary_bus_num = pci_read(&handle, 0x19, 8);
    struct pci_bus *child_bus = kzalloc(sizeof(struct pci_bus));
    if (child_bus) {
      child_bus->number = secondary_bus_num;
      child_bus->segment = bus->segment;
      child_bus->parent = bus;
      child_bus->bus_type = pci_bus_type;
      INIT_LIST_HEAD(&child_bus->devices);
      INIT_LIST_HEAD(&child_bus->children);
      list_add_tail(&child_bus->node, &bus->children);
      pci_enumerate_bus(child_bus);
    }
  }
}

static void __no_cfi subsys_enumerate_bus(struct pci_bus *bus) {
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

  // 0. Register Bus
  bus_register(&pci_bus_type);

  // 1. Register Subsystem Interface
  pci_register_subsystem(&subsys_ops);

  // 2. Initialize Backends
  pci_backend_pio_init();
  pci_backend_ecam_init();

  // 3. Create root bus 0 and scan
  struct pci_bus *root_bus = kzalloc(sizeof(struct pci_bus));
  if (!root_bus) return -ENOMEM;

  root_bus->number = 0;
  root_bus->segment = 0;
  root_bus->bus_type = pci_bus_type;
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
  nullptr
);
