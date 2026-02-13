/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/platform.h
 * @brief Platform Device Interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/sysintf/device.h>

struct platform_device;

/**
 * struct platform_device - device on the platform bus (SoC, Legacy)
 * @name: Name of the device
 * @id: Instance ID (-1 if unique)
 * @dev: Base device object
 * @num_resources: Number of resources
 * @resources: Array of resources (MMIO, IRQ)
 */
struct platform_device {
    struct device dev;
    const char *name;
    int id;
    
    uint32_t num_resources;
    struct resource *resources;
    
    const struct platform_device_id *id_entry;
};

struct platform_driver {
    int (*probe)(struct platform_device *);
    int (*remove)(struct platform_device *);
    void (*shutdown)(struct platform_device *);
    struct device_driver driver;
    const struct platform_device_id *id_table;
};

/* Platform Resources */
#define IORESOURCE_IO       0x00000100
#define IORESOURCE_MEM      0x00000200
#define IORESOURCE_IRQ      0x00000400
#define IORESOURCE_DMA      0x00000800

struct resource {
    uint64_t start;
    uint64_t end;
    const char *name;
    uint32_t flags;
    struct resource *parent, *sibling, *child;
};

/**
 * platform_device_register - add a platform-level device
 */
int platform_device_register(struct platform_device *pdev);
void platform_device_unregister(struct platform_device *pdev);

/**
 * platform_driver_register - register a driver for platform devices
 */
int platform_driver_register(struct platform_driver *drv);
void platform_driver_unregister(struct platform_driver *drv);

/**
 * platform_get_resource - retrieve resource from device
 */
struct resource *platform_get_resource(struct platform_device *dev, unsigned int type, unsigned int num);
int platform_get_irq(struct platform_device *dev, unsigned int num);
