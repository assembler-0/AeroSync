/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/acpi_dev.c
 * @brief ACPI Namespace Device Enumerator
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/classes.h>
#include <aerosync/sysintf/bus.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>

struct acpi_device {
  struct device dev;
  uacpi_namespace_node *node;
  char hid[16];
};

struct acpi_driver {
  const char *hid;
  struct device_driver driver;
  int (*probe)(struct acpi_device *dev);
};

#define to_acpi_dev(d) container_of(d, struct acpi_device, dev)
#define to_acpi_driver(dr) container_of(dr, struct acpi_driver, driver)

static int acpi_bus_match(struct device *dev, struct device_driver *drv) {
  struct acpi_device *adev = to_acpi_dev(dev);
  struct acpi_driver *adrv = to_acpi_driver(drv);

  if (adev->hid[0] && adrv->hid && strcmp(adev->hid, adrv->hid) == 0)
    return 1;
  return 0;
}

static int acpi_bus_probe(struct device *dev) {
  struct acpi_device *adev = to_acpi_dev(dev);
  struct acpi_driver *adrv = to_acpi_driver(dev->driver);

  if (adrv->probe)
    return adrv->probe(adev);
  return 0;
}

struct bus_type acpi_bus_type = {
  .name = "acpi",
  .match = acpi_bus_match,
  .probe = acpi_bus_probe,
};

static bool acpi_bus_registered = false;

static void acpi_dev_release(struct device *dev) {
  struct acpi_device *adev = to_acpi_dev(dev);
  kfree(adev);
}

static uacpi_iteration_decision
acpi_enum_callback(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
  (void) user;
  (void) depth;
  uacpi_namespace_node_info *info;
  uacpi_status st;

  st = uacpi_get_namespace_node_info(node, &info);
  if (uacpi_unlikely_error(st))
    return UACPI_ITERATION_DECISION_CONTINUE;

  if (info->type == UACPI_OBJECT_DEVICE) {
    struct acpi_device *adev = kzalloc(sizeof(struct acpi_device));
    if (!adev) {
      uacpi_free_namespace_node_info(info);
      return UACPI_ITERATION_DECISION_CONTINUE;
    }

    adev->node = node;
    device_initialize(&adev->dev);
    adev->dev.bus = &acpi_bus_type;
    adev->dev.release = acpi_dev_release;

    // Get name from node
    uacpi_object_name name = uacpi_namespace_node_name(node);
    const char *acpi_prefix = STRINGIFY(CONFIG_ACPI_NAME_PREFIX);
    if (acpi_prefix[0] != '\0') {
        device_set_name(&adev->dev, "%s_%c%c%c%c", acpi_prefix, name.text[0], name.text[1],
                 name.text[2], name.text[3]);
    } else {
        device_set_name(&adev->dev, "%c%c%c%c", name.text[0], name.text[1],
                 name.text[2], name.text[3]);
    }

    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) {
      strncpy(adev->hid, info->hid.value, 15);
    }

    if (device_add(&adev->dev) != 0) {
      kfree(adev);
    } else {
      printk(KERN_DEBUG ACPI_CLASS "discovered device %s (HID: %s)\n", adev->dev.name,
             adev->hid[0] ? adev->hid : "None");
    }
  }

  uacpi_free_namespace_node_info(info);
  return UACPI_ITERATION_DECISION_CONTINUE;
}

int acpi_bus_enumerate(void) {
  if (!acpi_bus_registered) {
    bus_register(&acpi_bus_type);
    acpi_bus_registered = true;
  }

  printk(KERN_INFO ACPI_CLASS "enumerating ACPI namespace...\n");
  uacpi_namespace_for_each_child(uacpi_namespace_root(), acpi_enum_callback,
                                 UACPI_NULL, UACPI_OBJECT_ANY_BIT,
                                 UACPI_MAX_DEPTH_ANY, nullptr);
  return 0;
}
