/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/acpi_dev.c
 * @brief ACPI Namespace Device Enumerator (Unified Model)
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

  /* If driver specifies a HID, it must match the device's Hardware ID */
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

static void acpi_dev_release(struct device *dev) {
  struct acpi_device *adev = to_acpi_dev(dev);
  kfree(adev);
}

static ACPI_STATUS __no_cfi acpi_enum_callback(ACPI_HANDLE object,
                                               uint32_t nesting_level,
                                               void *context,
                                               void **return_value) {
  (void)nesting_level; (void)context; (void)return_value;
  ACPI_DEVICE_INFO *info;
  if (ACPI_FAILURE(AcpiGetObjectInfo(object, &info))) return AE_OK;

  if (info->Type == ACPI_TYPE_DEVICE || info->Type == ACPI_TYPE_PROCESSOR) {
    struct acpi_device *adev = kzalloc(sizeof(struct acpi_device));
    if (!adev) { ACPI_FREE(info); return AE_OK; }

    device_initialize(&adev->dev);
    adev->handle = object;
    adev->dev.platform_data = object; /* Store handle for lookup */
    adev->dev.bus = &acpi_bus_type;
    adev->dev.release = acpi_dev_release;

    /* Topological Parenting: Find the parent device in the UDM tree by its handle */
    ACPI_HANDLE parent_handle;
    if (ACPI_SUCCESS(AcpiGetParent(object, &parent_handle))) {
        struct device *parent_dev = device_find_by_platform_data(parent_handle);
        if (parent_dev) {
            adev->dev.parent = parent_dev;
            /* device_add will handle get_device(parent) and list addition */
        }
    }

    ACPI_BUFFER buffer = {ACPI_ALLOCATE_BUFFER, nullptr};
    if (ACPI_SUCCESS(AcpiGetName(object, ACPI_SINGLE_NAME, &buffer))) {
        device_set_name(&adev->dev, "ACPI_%s", (char*)buffer.Pointer);
        if (buffer.Pointer) ACPI_FREE(buffer.Pointer);
    } else {
        device_set_name(&adev->dev, "ACPI_DEV_%p", object);
    }

    if (info->Valid & ACPI_VALID_HID) strncpy(adev->hid, info->HardwareId.String, 15);

    if (device_add(&adev->dev) != 0) {
        kfree(adev);
    } else {
        printk(KERN_DEBUG ACPI_CLASS "Discovered device %s (HID: %s)\n",
               adev->dev.name, adev->hid[0] ? adev->hid : "None");
    }
  }

  ACPI_FREE(info);
  return AE_OK;
}

int acpi_bus_enumerate(void) {
  static bool registered = false;
  if (!registered) {
    bus_register(&acpi_bus_type);
    registered = true;
  }
  
  printk(KERN_INFO ACPI_CLASS "Enumerating ACPI namespace...\n");
  AcpiWalkNamespace(ACPI_TYPE_ANY, ACPI_ROOT_OBJECT, ACPI_UINT32_MAX,
                    acpi_enum_callback, nullptr, nullptr, nullptr);
  return 0;
}
