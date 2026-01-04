/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file include/kernel/sysintf/device.h
 * @brief Unified Driver Model - Device and Driver structures
 * @copyright (C) 2025 assembler-0
 */

#pragma once

#include <kernel/types.h>
#include <linux/list.h>
#include <kernel/mutex.h>

struct device;
struct device_driver;
struct bus_type;

/**
 * struct device_driver - The basic driver structure
 */
struct device_driver {
    const char *name;
    struct bus_type *bus;

    int (*probe)(struct device *dev);
    void (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);

    struct list_head bus_node; /* node in bus_type->drivers_list */
};

/**
 * struct device - The basic device structure
 */
struct device {
    struct device *parent;
    const char *name;

    struct bus_type *bus;           /* type of bus device is on */
    struct device_driver *driver;    /* which driver has allocated this device */
    
    void *platform_data;            /* Platform specific data, eg. ACPI handle */
    void *driver_data;              /* Driver specific data */

    struct list_head node;          /* node in global device list */
    struct list_head bus_node;      /* node in bus_type->devices_list */
    struct list_head children;      /* list of child devices */
    struct list_head child_node;    /* node in parent->children list */

    void (*release)(struct device *dev);
};

/* --- Registration API --- */

/**
 * device_register - register a device with the system
 */
int device_register(struct device *dev);

/**
 * device_unregister - unregister a device from the system
 */
void device_unregister(struct device *dev);

/**
 * driver_register - register a driver with its bus
 */
int driver_register(struct device_driver *drv);

/**
 * driver_unregister - unregister a driver
 */
void driver_unregister(struct device_driver *drv);

/* --- Helpers --- */

static inline void dev_set_drvdata(struct device *dev, void *data) {
    dev->driver_data = data;
}

static inline void *dev_get_drvdata(const struct device *dev) {
    return dev->driver_data;
}
