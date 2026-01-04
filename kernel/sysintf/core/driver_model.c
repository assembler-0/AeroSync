///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sysintf/core/driver_model.c
 * @brief Unified Driver Model Implementation
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/sysintf/device.h> 
#include <kernel/sysintf/bus.h> 
#include <kernel/classes.h> 
#include <lib/printk.h> 
#include <linux/container_of.h>
#include <kernel/fkx/fkx.h> 
#include <kernel/errno.h> 

static LIST_HEAD(global_device_list);
static mutex_t device_model_lock;

/* Initialization of the core driver model */
static void driver_model_init(void) {
    static int initialized = 0;
    if (initialized) return;
    mutex_init(&device_model_lock);
    initialized = 1;
}

/* --- Bus Logic --- */

int bus_register(struct bus_type *bus) {
    driver_model_init();
    if (!bus || !bus->name) return -EINVAL;

    mutex_init(&bus->lock);
    INIT_LIST_HEAD(&bus->drivers_list);
    INIT_LIST_HEAD(&bus->devices_list);

    printk(KERN_DEBUG HAL_CLASS "Registered bus '%s'\n", bus->name);
    return 0;
}
EXPORT_SYMBOL(bus_register);

void bus_unregister(struct bus_type *bus) {
    if (!bus) return;
    /* TODO: Ensure all devices/drivers are gone */
    printk(KERN_DEBUG HAL_CLASS "Unregistered bus '%s'\n", bus->name);
}
EXPORT_SYMBOL(bus_unregister);

/* --- Device/Driver Matching --- */

static int device_bind_driver(struct device *dev) {
    int ret;

    if (dev->bus->probe) {
        ret = dev->bus->probe(dev);
    } else if (dev->driver->probe) {
        ret = dev->driver->probe(dev);
    } else {
        return -ENODEV;
    }

    if (ret == 0) {
        printk(KERN_INFO HAL_CLASS "Device '%s' bound to driver '%s'\n", 
               dev->name ? dev->name : "unnamed", dev->driver->name);
    }

    return ret;
}

static int device_attach_driver(struct device *dev) {
    struct device_driver *drv;
    int ret = -ENODEV;

    if (!dev->bus) return -EINVAL;

    mutex_lock(&dev->bus->lock);
    list_for_each_entry(drv, &dev->bus->drivers_list, bus_node) {
        /* 1. Does the bus think they match? */
        if (dev->bus->match && !dev->bus->match(dev, drv))
            continue;

        /* 2. Try to bind */
        dev->driver = drv;
        ret = device_bind_driver(dev);
        if (ret == 0) {
            goto out;
        }
        dev->driver = NULL;
    }

out:
    mutex_unlock(&dev->bus->lock);
    return ret;
}

/* --- Device Logic --- */

int device_register(struct device *dev) {
    driver_model_init();
    if (!dev) return -EINVAL;

    INIT_LIST_HEAD(&dev->children);
    
    mutex_lock(&device_model_lock);
    list_add_tail(&dev->node, &global_device_list);
    if (dev->parent) {
        list_add_tail(&dev->child_node, &dev->parent->children);
    }
    mutex_unlock(&device_model_lock);

    if (dev->bus) {
        mutex_lock(&dev->bus->lock);
        list_add_tail(&dev->bus_node, &dev->bus->devices_list);
        mutex_unlock(&dev->bus->lock);

        /* Try to find a driver */
        device_attach_driver(dev);
    }

    return 0;
}
EXPORT_SYMBOL(device_register);

void device_unregister(struct device *dev) {
    if (!dev) return;

    if (dev->driver) {
        if (dev->bus && dev->bus->remove)
            dev->bus->remove(dev);
        else if (dev->driver->remove)
            dev->driver->remove(dev);
        dev->driver = NULL;
    }

    if (dev->bus) {
        mutex_lock(&dev->bus->lock);
        list_del(&dev->bus_node);
        mutex_unlock(&dev->bus->lock);
    }

    mutex_lock(&device_model_lock);
    list_del(&dev->node);
    if (dev->parent) {
        list_del(&dev->child_node);
    }
    mutex_unlock(&device_model_lock);

    if (dev->release)
        dev->release(dev);
}
EXPORT_SYMBOL(device_unregister);

/* --- Driver Logic --- */

int driver_register(struct device_driver *drv) {
    driver_model_init();
    if (!drv || !drv->bus) return -EINVAL;

    mutex_lock(&drv->bus->lock);
    list_add_tail(&drv->bus_node, &drv->bus->drivers_list);
    mutex_unlock(&drv->bus->lock);

    /* Try to bind this driver to existing devices on the bus */
    struct device *dev;
    mutex_lock(&drv->bus->lock);
    list_for_each_entry(dev, &drv->bus->devices_list, bus_node) {
        if (!dev->driver) {
            if (drv->bus->match && drv->bus->match(dev, drv)) {
                dev->driver = drv;
                if (device_bind_driver(dev) != 0) {
                    dev->driver = NULL;
                }
            }
        }
    }
    mutex_unlock(&drv->bus->lock);

    return 0;
}
EXPORT_SYMBOL(driver_register);

void driver_unregister(struct device_driver *drv) {
    if (!drv || !drv->bus) return;

    struct device *dev;
    mutex_lock(&drv->bus->lock);
    list_for_each_entry(dev, &drv->bus->devices_list, bus_node) {
        if (dev->driver == drv) {
            if (drv->bus->remove)
                drv->bus->remove(dev);
            else if (drv->remove)
                drv->remove(dev);
            dev->driver = NULL;
        }
    }
    list_del(&drv->bus_node);
    mutex_unlock(&drv->bus->lock);
}
EXPORT_SYMBOL(driver_unregister);
