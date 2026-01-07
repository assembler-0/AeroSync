/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/kernel/sysintf/bus.h
 * @brief Unified Driver Model - Bus structure
 * @copyright (C) 2025 assembler-0
 */

#pragma once

#include <kernel/types.h>
#include <linux/list.h>
#include <kernel/mutex.h>

struct device;
struct device_driver;

/**
 * struct bus_type - The bus type structure
 */
struct bus_type {
    const char *name;

    /**
     * match - callback to determine if a driver can handle a device
     */
    int (*match)(struct device *dev, struct device_driver *drv);

    /**
     * probe - default probe for the bus
     */
    int (*probe)(struct device *dev);

    /**
     * remove - default remove for the bus
     */
    void (*remove)(struct device *dev);

    struct list_head drivers_list;
    struct list_head devices_list;
    
    mutex_t lock;
};

/**
 * bus_register - register a bus type
 */
int bus_register(struct bus_type *bus);

/**
 * bus_unregister - unregister a bus type
 */
void bus_unregister(struct bus_type *bus);

/**
 * bus_for_each_dev - device iterator
 */
int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
                    int (*fn)(struct device *, void *));

/**
 * bus_for_each_drv - driver iterator
 */
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start, void *data,
                    int (*fn)(struct device_driver *, void *));
