/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/core/platform.c
 * @brief Platform Bus Implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sysintf/platform.h>
#include <aerosync/sysintf/bus.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <linux/container_of.h>

/* --- Platform Bus Definition --- */

static int platform_match(struct device *dev, struct device_driver *drv) {
  /* Simple name matching for now */
  struct platform_device *pdev = container_of(dev, struct platform_device, dev);
  return (strcmp(pdev->name, drv->name) == 0);
}

static int platform_probe(struct device *dev) {
  struct platform_driver *pdrv = container_of(dev->driver, struct platform_driver, driver);
  struct platform_device *pdev = container_of(dev, struct platform_device, dev);

  if (pdrv->probe) {
    return pdrv->probe(pdev);
  }
  return 0;
}

static void platform_remove(struct device *dev) {
  struct platform_driver *pdrv = container_of(dev->driver, struct platform_driver, driver);
  struct platform_device *pdev = container_of(dev, struct platform_device, dev);

  if (pdrv->remove) {
    pdrv->remove(pdev);
  }
}

static void platform_shutdown(struct device *dev) {
  struct platform_driver *pdrv = container_of(dev->driver, struct platform_driver, driver);
  struct platform_device *pdev = container_of(dev, struct platform_device, dev);

  if (pdrv->shutdown) {
    pdrv->shutdown(pdev);
  }
}

struct bus_type platform_bus_type = {
  .name = "platform",
  .match = platform_match,
  .probe = platform_probe,
  .remove = platform_remove,
};

static int platform_bus_initialized = 0;

static void platform_bus_init(void) {
  if (!platform_bus_initialized) {
    bus_register(&platform_bus_type);
    platform_bus_initialized = 1;
  }
}

/* --- Platform Device API --- */

static void platform_device_release(struct device *dev) {
  /*
   * In Linux, this usually frees the pdev structure.
   * We don't auto-free dynamic pdevs yet unless we add alloc/register split.
   * For now, we assume pdev is either static or managed by caller.
   */
}

int platform_device_register(struct platform_device *pdev) {
  platform_bus_init();

  if (!pdev) return -EINVAL;

  pdev->dev.bus = &platform_bus_type;
  pdev->dev.release = platform_device_release;

  if (pdev->id != -1) {
    device_set_name(&pdev->dev, "%s.%d", pdev->name, pdev->id);
  } else {
    device_set_name(&pdev->dev, "%s", pdev->name);
  }

  return device_register(&pdev->dev);
}

EXPORT_SYMBOL(platform_device_register);

void platform_device_unregister(struct platform_device *pdev) {
  if (pdev) {
    device_unregister(&pdev->dev);
  }
}

EXPORT_SYMBOL(platform_device_unregister);

/* --- Platform Driver API --- */

int platform_driver_register(struct platform_driver *drv) {
  platform_bus_init();

  if (!drv) return -EINVAL;

  drv->driver.bus = &platform_bus_type;
  /* Name must be set by caller in drv->driver.name or we copy it */
  if (!drv->driver.name) {
    // Fallback or error? Assuming caller sets generic driver name matching pdev name
    return -EINVAL;
  }

  return driver_register(&drv->driver);
}

EXPORT_SYMBOL(platform_driver_register);

void platform_driver_unregister(struct platform_driver *drv) {
  driver_unregister(&drv->driver);
}

EXPORT_SYMBOL(platform_driver_unregister);

/* --- Resource API --- */

struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num) {
  if (!dev) return nullptr;

  unsigned int n = 0;
  for (uint32_t i = 0; i < dev->num_resources; i++) {
    struct resource *r = &dev->resources[i];
    if (type == (r->flags & (IORESOURCE_IO | IORESOURCE_MEM | IORESOURCE_IRQ | IORESOURCE_DMA))) {
      if (n++ == num) {
        return r;
      }
    }
  }
  return nullptr;
}

EXPORT_SYMBOL(platform_get_resource);

int platform_get_irq(struct platform_device *dev, unsigned int num) {
  struct resource *r = platform_get_resource(dev, IORESOURCE_IRQ, num);
  if (r) {
    return (int) r->start;
  }
  return -ENXIO;
}

EXPORT_SYMBOL(platform_get_irq);
