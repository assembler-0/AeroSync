/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/acpi_dev.c
 * @brief ACPI Namespace Device Enumerator using ACPICA
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sysintf/acpi.h>
#include <aerosync/sysintf/bus.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/slub.h>

struct acpi_device {
  struct device dev;
  ACPI_HANDLE handle;
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

static int __no_cfi acpi_bus_probe(struct device *dev) {
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

static ACPI_STATUS __no_cfi acpi_enum_callback(ACPI_HANDLE object,
                                               uint32_t nesting_level,
                                               void *context,
                                               void **return_value) {
  (void)nesting_level;
  (void)context;
  (void)return_value;

  ACPI_DEVICE_INFO *info;
  ACPI_STATUS st;

  st = AcpiGetObjectInfo(object, &info);
  if (ACPI_FAILURE(st))
    return AE_OK;

  if (info->Type == ACPI_TYPE_DEVICE) {
    struct acpi_device *adev = kzalloc(sizeof(struct acpi_device));
    if (!adev) {
      ACPI_FREE(info);
      return AE_OK;
    }

    adev->handle = object;
    device_initialize(&adev->dev);
    adev->dev.bus = &acpi_bus_type;
    adev->dev.release = acpi_dev_release;

    // Get name from handle
    ACPI_BUFFER buffer = {ACPI_ALLOCATE_BUFFER, nullptr};
    st = AcpiGetName(object, ACPI_SINGLE_NAME, &buffer);

    const char *name_ptr =
        (st == AE_OK) ? (const char *)buffer.Pointer : "????";
    const char *acpi_prefix = CONFIG_ACPI_NAME_PREFIX;

    if (acpi_prefix[0] != '\0') {
      device_set_name(&adev->dev, "%s_%s", acpi_prefix, name_ptr);
    } else {
      device_set_name(&adev->dev, "%s", name_ptr);
    }

    if (buffer.Pointer)
      ACPI_FREE(buffer.Pointer);

    if (info->Valid & ACPI_VALID_HID) {
      strncpy(adev->hid, info->HardwareId.String, 15);
    }

    if (device_add(&adev->dev) != 0) {
      kfree(adev);
    } else {
      printk(ACPI_CLASS "discovered device %s (HID: %s)\n",
             adev->dev.name, adev->hid[0] ? adev->hid : "None");
    }
  }

  ACPI_FREE(info);
  return AE_OK;
}

int acpi_bus_enumerate(void) {
  if (!acpi_bus_registered) {
    bus_register(&acpi_bus_type);
    acpi_bus_registered = true;
  }

  printk(KERN_INFO ACPI_CLASS "enumerating ACPI namespace...\n");
  AcpiWalkNamespace(ACPI_TYPE_DEVICE, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX,
                    acpi_enum_callback, nullptr, nullptr, nullptr);
  return 0;
}
