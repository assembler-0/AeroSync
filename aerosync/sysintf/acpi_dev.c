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
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/vsprintf.h>
#include <mm/slub.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>

static struct class acpi_class = {
  .name = "acpi_bus",
};

static bool acpi_class_registered = false;

struct acpi_device {
  struct device dev;
  uacpi_namespace_node *node;
  char hid[16];
};

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
    adev->dev.class = &acpi_class;

    // Get name from node
    uacpi_object_name name = uacpi_namespace_node_name(node);
    char name_buf[16];
    snprintf(name_buf, 16, "acpi_%c%c%c%c", name.text[0], name.text[1],
             name.text[2], name.text[3]);

    char *final_name = kzalloc(16);
    if (final_name) {
      memcpy(final_name, name_buf, 16);
      adev->dev.name = final_name;
    } else {
      adev->dev.name = "acpi_dev";
    }

    if (info->flags & UACPI_NS_NODE_INFO_HAS_HID) {
      strncpy(adev->hid, info->hid.value, 15);
    }

    if (device_register(&adev->dev) != 0) {
      if (final_name)
        kfree(final_name);
      kfree(adev);
    } else {
      printk(KERN_DEBUG ACPI_CLASS "discovered device %s (HID: %s)\n", name_buf,
             adev->hid[0] ? adev->hid : "None");
    }
  }

  uacpi_free_namespace_node_info(info);
  return UACPI_ITERATION_DECISION_CONTINUE;
}

int acpi_bus_enumerate(void) {
  if (!acpi_class_registered) {
    class_register(&acpi_class);
    acpi_class_registered = true;
  }

  printk(KERN_INFO ACPI_CLASS "enumerating ACPI namespace...\n");
  uacpi_namespace_for_each_child(uacpi_namespace_root(), acpi_enum_callback,
                                 UACPI_NULL, UACPI_OBJECT_ANY_BIT,
                                 UACPI_MAX_DEPTH_ANY, NULL);
  return 0;
}
