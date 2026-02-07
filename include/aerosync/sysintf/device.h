/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/device.h
 * @brief Unified Driver Model - Device and Driver structures
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/mutex.h>
#include <aerosync/types.h>
#include <aerosync/kref.h>
#include <aerosync/sysintf/attribute.h>
#include <linux/list.h>

struct device;
struct device_driver;
struct bus_type;
struct class;

/**
 * struct device_driver - The basic driver structure
 */
struct device_driver {
  const char *name;
  struct bus_type *bus;

  int (*probe)(struct device *dev);
  void (*remove)(struct device *dev);
  void (*shutdown)(struct device *dev);
  int (*suspend)(struct device *dev);
  int (*resume)(struct device *dev);

  const struct attribute_group **groups; /* Default attributes */

  struct list_head bus_node; /* node in bus_type->drivers_list */
};

/**
 * struct device - The basic device structure
 */
struct device {
  struct kref kref;             /* reference count */
  struct device *parent;
  const char *name;
  bool name_allocated;          /* is name allocated? */
  int id;                       /* device id on bus or class */
  bool class_id_allocated;      /* was id allocated from class ida? */

  struct bus_type *bus;         /* type of bus device is on */
  struct device_driver *driver; /* which driver has allocated this device */

  void *platform_data; /* Platform specific data, eg. ACPI handle */
  void *driver_data;   /* Driver specific data */

  /* Managed Resources (devres) */
  struct list_head devres_head;
  mutex_t devres_lock;

  /* Attributes */
  const struct attribute_group **groups; /* Attributes for this device */

  struct list_head node;       /* node in global device list */
  struct list_head bus_node;   /* node in bus_type->devices_list */
  struct list_head children;   /* list of child devices */
  struct list_head child_node; /* node in parent->children list */

  struct class *class;         /* class this device belongs to */
  struct list_head class_node; /* node in class->devices list */

  void (*release)(struct device *dev);
};

/* --- Managed Resources (devres) --- */

typedef void (*dr_release_t)(struct device *dev, void *res);

struct devres {
  struct list_head entry;
  dr_release_t release;
  const char *name;
  size_t size;
  /* Data follows */
};

/**
 * devres_alloc - Allocate managed resource
 */
void *devres_alloc(dr_release_t release, size_t size, const char *name);

/**
 * devres_free - Free managed resource
 */
void devres_free(void *res);

/**
 * devres_add - Add managed resource to device
 */
void devres_add(struct device *dev, void *res);

/**
 * devres_release_all - Release all managed resources for a device
 */
void devres_release_all(struct device *dev);

/* Helper: devm_kzalloc */
void *devm_kzalloc(struct device *dev, size_t size);

/**
 * devm_ioremap - Managed ioremap
 */
void *devm_ioremap(struct device *dev, uint64_t phys_addr, size_t size);

/**
 * devm_request_irq - Managed IRQ request
 * @note This is a simplified version for now using vectors directly on x86
 */
int devm_request_irq(struct device *dev, uint8_t vector, void (*handler)(void *regs), const char *name, void *dev_id);

/* --- Registration API --- */

/**
 * device_initialize - init device structure (kref, lists)
 */
void device_initialize(struct device *dev);

/**
 * device_add - add device to system (bus, class, parent)
 */
int device_add(struct device *dev);

/**
 * device_register - register a device with the system (init + add)
 */
int device_register(struct device *dev);

/**
 * device_unregister - unregister a device from the system
 */
void device_unregister(struct device *dev);

/**
 * get_device - increment reference count
 */
struct device *get_device(struct device *dev);

/**
 * put_device - decrement reference count
 */
void put_device(struct device *dev);

/**
 * device_set_name - set the name of the device
 */
int device_set_name(struct device *dev, const char *fmt, ...);

/**
 * device_find_by_name - find a device by its name
 * Returns a reference to the device (must be put_device'd)
 */
struct device *device_find_by_name(const char *name);

/**
 * device_create_file - Create an attribute file (fake sysfs)
 */
int device_create_file(struct device *dev, const struct device_attribute *attr);

/**
 * device_remove_file - Remove an attribute file
 */
void device_remove_file(struct device *dev, const struct device_attribute *attr);

/**
 * driver_register - register a driver with its bus
 */
int driver_register(struct device_driver *drv);

/**
 * driver_unregister - unregister a driver
 */
void driver_unregister(struct device_driver *drv);

/**
 * dump_device_tree - Print the entire device hierarchy to kernel log
 */
void dump_device_tree(void);

/* --- Helpers --- */

static inline void dev_set_drvdata(struct device *dev, void *data) {
  dev->driver_data = data;
}

static inline void *dev_get_drvdata(const struct device *dev) {
  return dev->driver_data;
}
