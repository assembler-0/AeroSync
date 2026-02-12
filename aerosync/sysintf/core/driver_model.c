///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/core/driver_model.c
 * @brief Unified Driver Model Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sysintf/bus.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/block.h>
#include <lib/printk.h>
#include <lib/vsprintf.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <aerosync/kref.h>
#include <mm/slub.h>
#include <stdarg.h>
#include <mm/vmalloc.h>
#include <arch/x86_64/irq.h>

static LIST_HEAD(global_device_list);
static LIST_HEAD(global_class_list);
static mutex_t device_model_lock;

/* Initialization of the core driver model */
static void driver_model_init(void) {
  static int initialized = 0;
  if (initialized)
    return;
  mutex_init(&device_model_lock);
  initialized = 1;
}

/* --- Bus Logic --- */

int bus_register(struct bus_type *bus) {
  driver_model_init();
  if (!bus || !bus->name)
    return -EINVAL;

  mutex_init(&bus->lock);
  INIT_LIST_HEAD(&bus->drivers_list);
  INIT_LIST_HEAD(&bus->devices_list);

  printk(KERN_DEBUG HAL_CLASS "Registered bus '%s'\n", bus->name);
  return 0;
}

EXPORT_SYMBOL(bus_register);

void bus_unregister(struct bus_type *bus) {
  if (!bus)
    return;

  /* 1. Unregister all drivers on this bus */
  struct device_driver *drv, *tmp_drv;
  mutex_lock(&bus->lock);
  list_for_each_entry_safe(drv, tmp_drv, &bus->drivers_list, bus_node) {
    mutex_unlock(&bus->lock);
    driver_unregister(drv);
    mutex_lock(&bus->lock);
  }

  /* 2. Unregister all devices on this bus */
  struct device *dev, *tmp_dev;
  list_for_each_entry_safe(dev, tmp_dev, &bus->devices_list, bus_node) {
    mutex_unlock(&bus->lock);
    device_unregister(dev);
    mutex_lock(&bus->lock);
  }
  mutex_unlock(&bus->lock);

  printk(KERN_DEBUG HAL_CLASS "Unregistered bus '%s'\n", bus->name);
}

EXPORT_SYMBOL(bus_unregister);

/* --- Class Logic --- */

int class_register(struct class *cls) {
  driver_model_init();
  if (!cls || !cls->name)
    return -EINVAL;

  mutex_init(&cls->lock);
  INIT_LIST_HEAD(&cls->devices);

  /* Initialize ID allocator if any naming mechanism is provided */
  if (cls->dev_name || cls->dev_prefix || cls->naming_scheme != NAMING_NONE) {
    ida_init(&cls->ida, 1024); // Support up to 1024 devices per class
  }

  mutex_lock(&device_model_lock);
  list_add_tail(&cls->node, &global_class_list);
  mutex_unlock(&device_model_lock);

  printk(KERN_DEBUG HAL_CLASS "Registered class '%s'\n", cls->name);
  return 0;
}

EXPORT_SYMBOL(class_register);

void class_unregister(struct class *cls) {
  if (!cls)
    return;

  /* Unregister all devices in this class */
  struct device *dev, *tmp;
  mutex_lock(&cls->lock);
  list_for_each_entry_safe(dev, tmp, &cls->devices, class_node) {
    mutex_unlock(&cls->lock);
    device_unregister(dev);
    mutex_lock(&cls->lock);
  }
  mutex_unlock(&cls->lock);

  mutex_lock(&device_model_lock);
  list_del(&cls->node);
  mutex_unlock(&device_model_lock);

  if (cls->dev_name) {
    ida_destroy(&cls->ida);
  }

  printk(KERN_DEBUG HAL_CLASS "Unregistered class '%s'\n", cls->name);
}

EXPORT_SYMBOL(class_unregister);

int class_for_each_dev(struct class *cls, struct device *start, void *data,
                       class_iter_fn func) {
  if (!cls)
    return -EINVAL;

  struct device *dev;
  int ret = 0;

  mutex_lock(&cls->lock);
  list_for_each_entry(dev, &cls->devices, class_node) {
    if (start) {
      if (dev == start)
        start = nullptr;
      continue;
    }
    ret = func(dev, data);
    if (ret != 0)
      break;
  }
  mutex_unlock(&cls->lock);
  return ret;
}

EXPORT_SYMBOL(class_for_each_dev);

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

  if (!dev->bus)
    return -EINVAL;

  mutex_lock(&dev->bus->lock);
  list_for_each_entry(drv, &dev->bus->drivers_list, bus_node) {
    /* 1. Does the bus think they match? */
    if (dev->bus->match && !dev->bus->match(dev, drv))
      continue;

    /* 2. Try to bind */
    dev->driver = drv;
    ret = device_bind_driver(dev);
    if (likely(ret == 0)) {
      goto out;
    }
    dev->driver = nullptr;
  }

out:
  mutex_unlock(&dev->bus->lock);
  return ret;
}

/* --- Device Logic --- */

static void device_release_kref(struct kref *kref) {
  struct device *dev = container_of(kref, struct device, kref);

  if (dev->release)
    dev->release(dev);
  else
    printk(KERN_WARNING HAL_CLASS "Device '%s' does not have a release() function, it is broken and must be fixed.\n",
           dev->name ? dev->name : "(unknown)");

  if (dev->name_allocated && dev->name) {
    kfree((void *) dev->name);
    dev->name = nullptr;
  }
}

void device_initialize(struct device *dev) {
  kref_init(&dev->kref);
  INIT_LIST_HEAD(&dev->node);
  INIT_LIST_HEAD(&dev->bus_node);
  INIT_LIST_HEAD(&dev->children);
  INIT_LIST_HEAD(&dev->child_node);
  INIT_LIST_HEAD(&dev->class_node);
  INIT_LIST_HEAD(&dev->devres_head);
  mutex_init(&dev->devres_lock);
  dev->id = -1;
  dev->class_id_allocated = false;
  if (!dev->name)
    dev->name_allocated = false;
}

EXPORT_SYMBOL(device_initialize);

struct device *get_device(struct device *dev) {
  if (dev) {
    kref_get(&dev->kref);
  }
  return dev;
}

EXPORT_SYMBOL(get_device);

void put_device(struct device *dev) {
  if (dev) {
    kref_put(&dev->kref, device_release_kref);
  }
}

EXPORT_SYMBOL(put_device);

int device_set_name(struct device *dev, const char *fmt, ...) {
  va_list vargs;
  if (dev->name_allocated && dev->name) {
    kfree((void *) dev->name);
    dev->name = nullptr;
  }

  va_start(vargs, fmt);
  char *name = kvasprintf(fmt, vargs);
  va_end(vargs);

  if (!name) return -ENOMEM;

  dev->name = name;
  dev->name_allocated = true;
  return 0;
}

EXPORT_SYMBOL(device_set_name);

#include <fs/devfs.h>

static void generate_device_name(struct device *dev) {
  if (!dev->class || dev->name) return;

  int id = dev->id;
  if (id < 0) {
    id = ida_alloc(&dev->class->ida);
    if (id < 0) return;
    dev->id = id;
    dev->class_id_allocated = true;
  }

  const char *prefix = dev->class->dev_prefix;
  if (!prefix) prefix = dev->class->name;

  char name[64];
  if (dev->class->naming_scheme == NAMING_ALPHABETIC) {
    int len = strlen(prefix);
    strncpy(name, prefix, 60);
    if (id < 26) {
      name[len] = 'a' + id;
      name[len + 1] = '\0';
    } else {
      int first = (id / 26) - 1;
      int second = id % 26;
      name[len] = 'a' + first;
      name[len + 1] = 'a' + second;
      name[len + 2] = '\0';
    }
  } else if (dev->class->naming_scheme == NAMING_NUMERIC) {
    snprintf(name, sizeof(name), "%s%d", prefix, id);
  } else if (dev->class->dev_name) {
    /* Fallback to legacy printf-style template if provided */
    snprintf(name, sizeof(name), dev->class->dev_name, id);
  } else {
    return;
  }

  device_set_name(dev, "%s", name);
}

int device_add(struct device *dev) {
  driver_model_init();
  if (!dev)
    return -EINVAL;

  /* Class Registration & Naming */
  if (dev->class) {
    mutex_lock(&dev->class->lock);

    generate_device_name(dev);

    list_add_tail(&dev->class_node, &dev->class->devices);
    mutex_unlock(&dev->class->lock);

    if (dev->class->dev_probe) {
      if (dev->class->dev_probe(dev) != 0) {
        printk(KERN_ERR HAL_CLASS "Class probe failed for device '%s'\n",
               dev->name ? dev->name : "(unnamed)");
      }
    }
  }

  if (dev->parent)
    get_device(dev->parent);

  mutex_lock(&device_model_lock);
  list_add_tail(&dev->node, &global_device_list);
  if (dev->parent) {
    list_add_tail(&dev->child_node, &dev->parent->children);
  }
  mutex_unlock(&device_model_lock);

  /* Automatic devfs exposure */
  if (dev->class && (dev->class->flags & CLASS_FLAG_AUTO_DEVFS) && dev->name) {
    dev_t rdev = 0;
    vfs_mode_t mode = 0;

    switch (dev->class->category) {
      case DEV_CAT_CHAR:
      case DEV_CAT_TTY:
      case DEV_CAT_FB: {
        struct char_device *cdev = container_of(dev, struct char_device, dev);
        rdev = cdev->dev_num;
        mode = S_IFCHR | 0666;
        break;
      }
      case DEV_CAT_BLOCK: {
        struct block_device *bdev = container_of(dev, struct block_device, dev);
        rdev = bdev->dev_num;
        mode = S_IFBLK | 0660;
        break;
      }

      default:
        break;
    }

    if (mode != 0) {
      devfs_register_device(dev->name, mode, rdev, nullptr, nullptr);
    }
  }
  /* Create default attributes */
  if (dev->groups) {
    for (int i = 0; dev->groups[i]; i++) {
      const struct attribute_group *grp = dev->groups[i];
      for (int j = 0; grp->attrs[j]; j++) {
        /* In a real system, this would create sysfs nodes */
        printk(KERN_DEBUG HAL_CLASS "Created attribute '%s/%s'\n", dev->name, grp->attrs[j]->name);
      }
    }
  }

  if (dev->bus) {
    mutex_lock(&dev->bus->lock);
    list_add_tail(&dev->bus_node, &dev->bus->devices_list);
    mutex_unlock(&dev->bus->lock);

    /* Try to find a driver */
    device_attach_driver(dev);
  }

  return 0;
}

EXPORT_SYMBOL(device_add);

int device_register(struct device *dev) {
  device_initialize(dev);
  return device_add(dev);
}

EXPORT_SYMBOL(device_register);

/* --- Attribute Support --- */

int device_create_file(struct device *dev, const struct device_attribute *attr) {
  if (!dev || !attr) return -EINVAL;
  /*
   * In a full implementation, this would call sysfs_create_file(&dev->kobj, &attr->attr);
   * For now, we just validate it exists.
   */
  return 0;
}

EXPORT_SYMBOL(device_create_file);

void device_remove_file(struct device *dev, const struct device_attribute *attr) {
  /* Placeholder */
}

EXPORT_SYMBOL(device_remove_file);

void device_del(struct device *dev) {
  /* Release all managed resources before detaching */
  devres_release_all(dev);

  /* Remove attributes */
  /* sysfs_remove_groups(&dev->kobj, dev->groups); */

  if (dev->driver) {
    if (dev->bus && dev->bus->remove)
      dev->bus->remove(dev);
    else if (dev->driver->remove)
      dev->driver->remove(dev);
    dev->driver = nullptr;
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

  if (dev->class) {
    if (dev->class->dev_release)
      dev->class->dev_release(dev);

    mutex_lock(&dev->class->lock);
    if (dev->class_id_allocated) {
      ida_free(&dev->class->ida, dev->id);
      dev->class_id_allocated = false;
    }
    list_del(&dev->class_node);
    mutex_unlock(&dev->class->lock);
  }

  if (dev->parent)
    put_device(dev->parent);
}

void device_unregister(struct device *dev) {
  if (!dev)
    return;

  device_del(dev);
  put_device(dev);
}

EXPORT_SYMBOL(device_unregister);

/* --- Driver Logic --- */

int driver_register(struct device_driver *drv) {
  driver_model_init();
  if (!drv || !drv->bus)
    return -EINVAL;

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
          dev->driver = nullptr;
        }
      }
    }
  }
  mutex_unlock(&drv->bus->lock);

  return 0;
}

EXPORT_SYMBOL(driver_register);

void driver_unregister(struct device_driver *drv) {
  if (!drv || !drv->bus)
    return;

  struct device *dev;
  mutex_lock(&drv->bus->lock);
  list_for_each_entry(dev, &drv->bus->devices_list, bus_node) {
    if (dev->driver == drv) {
      if (drv->bus->remove)
        drv->bus->remove(dev);
      else if (drv->remove)
        drv->remove(dev);
      dev->driver = nullptr;
    }
  }
  list_del(&drv->bus_node);
  mutex_unlock(&drv->bus->lock);
}

EXPORT_SYMBOL(driver_unregister);

struct device *device_find_by_name(const char *name) {
  struct device *dev;
  struct device *found = nullptr;

  mutex_lock(&device_model_lock);
  list_for_each_entry(dev, &global_device_list, node) {
    if (dev->name && strcmp(dev->name, name) == 0) {
      found = get_device(dev);
      break;
    }
  }
  mutex_unlock(&device_model_lock);
  return found;
}

EXPORT_SYMBOL(device_find_by_name);

int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
                     int (*func)(struct device *, void *)) {
  struct device *dev;
  int error = 0;

  if (!bus)
    return -EINVAL;

  mutex_lock(&bus->lock);
  list_for_each_entry(dev, &bus->devices_list, bus_node) {
    if (start) {
      if (dev == start)
        start = nullptr;
      continue;
    }
    error = func(dev, data);
    if (error)
      break;
  }
  mutex_unlock(&bus->lock);
  return error;
}

EXPORT_SYMBOL(bus_for_each_dev);

int bus_for_each_drv(struct bus_type *bus, struct device_driver *start, void *data,
                     int (*func)(struct device_driver *, void *)) {
  struct device_driver *drv;
  int error = 0;

  if (!bus)
    return -EINVAL;

  mutex_lock(&bus->lock);
  list_for_each_entry(drv, &bus->drivers_list, bus_node) {
    if (start) {
      if (drv == start)
        start = nullptr;
      continue;
    }
    error = func(drv, data);
    if (error)
      break;
  }
  mutex_unlock(&bus->lock);
  return error;
}

EXPORT_SYMBOL(bus_for_each_drv);

static void dump_device_recursive(struct device *dev, int depth) {
  char indent[32];
  int i;
  for (i = 0; i < depth && i < 30; i++) {
    indent[i] = ' ';
  }
  indent[i] = '\0';

  const char *class_name = dev->class ? dev->class->name : "none";
  const char *driver_name = dev->driver ? dev->driver->name : "none";

  printk(KERN_INFO HAL_CLASS "%s|- %s [class: %s, driver: %s]\n",
         indent, dev->name ? dev->name : "(unnamed)", class_name, driver_name);

  struct device *child;
  list_for_each_entry(child, &dev->children, child_node) {
    dump_device_recursive(child, depth + 2);
  }
}

void dump_device_tree(void) {
  struct device *dev;
  printk(KERN_INFO HAL_CLASS "[--- system device tree ---\n");
  mutex_lock(&device_model_lock);
  list_for_each_entry(dev, &global_device_list, node) {
    if (!dev->parent) {
      dump_device_recursive(dev, 0);
    }
  }
  mutex_unlock(&device_model_lock);
}

EXPORT_SYMBOL(dump_device_tree);

/* --- Managed Resources (devres) Implementation --- */

void *devres_alloc(dr_release_t release, size_t size, const char *name) {
  struct devres *dr;

  dr = kzalloc(sizeof(struct devres) + size);
  if (!dr)
    return nullptr;

  INIT_LIST_HEAD(&dr->entry);
  dr->release = release;
  dr->name = name;
  dr->size = size;

  return (void *) (dr + 1);
}

EXPORT_SYMBOL(devres_alloc);

void devres_free(void *res) {
  if (res) {
    struct devres *dr = (struct devres *) res - 1;
    kfree(dr);
  }
}

EXPORT_SYMBOL(devres_free);

void devres_add(struct device *dev, void *res) {
  struct devres *dr = (struct devres *) res - 1;

  mutex_lock(&dev->devres_lock);
  list_add_tail(&dr->entry, &dev->devres_head);
  mutex_unlock(&dev->devres_lock);
}

EXPORT_SYMBOL(devres_add);

void devres_release_all(struct device *dev) {
  struct devres *dr, *tmp;

  mutex_lock(&dev->devres_lock);
  list_for_each_entry_safe(dr, tmp, &dev->devres_head, entry) {
    list_del(&dr->entry);
    mutex_unlock(&dev->devres_lock);

    if (dr->release) {
      dr->release(dev, (void *) (dr + 1));
    }
    kfree(dr);

    mutex_lock(&dev->devres_lock);
  }
  mutex_unlock(&dev->devres_lock);
}

EXPORT_SYMBOL(devres_release_all);

static void devm_kzalloc_release(struct device *dev, void *res) {
  /* kzalloc'd memory is part of the devres struct, so nothing to do here
   * as devres_release_all frees the whole struct devres. */
  (void) dev;
  (void) res;
}

void *devm_kzalloc(struct device *dev, size_t size) {
  void *ptr = devres_alloc(devm_kzalloc_release, size, "devm_kzalloc");
  if (ptr) {
    devres_add(dev, ptr);
  }
  return ptr;
}

EXPORT_SYMBOL(devm_kzalloc);

static void devm_ioremap_release(struct device *dev, void *res) {
  (void) dev;
  iounmap(*(void **) res);
}

void *devm_ioremap(struct device *dev, uint64_t phys_addr, size_t size) {
  void **ptr = devres_alloc(devm_ioremap_release, sizeof(void *), "devm_ioremap");
  if (!ptr)
    return nullptr;

  *ptr = ioremap(phys_addr, size);
  if (!*ptr) {
    devres_free(ptr);
    return nullptr;
  }

  devres_add(dev, ptr);
  return *ptr;
}

EXPORT_SYMBOL(devm_ioremap);

struct devm_irq_res {
  uint8_t vector;
};

static void devm_irq_release(struct device *dev, void *res) {
  struct devm_irq_res *irq_res = res;
  (void) dev;
  irq_uninstall_handler(irq_res->vector);
}

int devm_request_irq(struct device *dev, uint8_t vector, void (*handler)(void *regs),
                     const char *name, void *dev_id) {
  (void) name;
  (void) dev_id;
  struct devm_irq_res *dr = devres_alloc(devm_irq_release, sizeof(struct devm_irq_res), "devm_irq");
  if (!dr)
    return -ENOMEM;

  dr->vector = vector;
  irq_install_handler(vector, (irq_handler_t) handler);

  devres_add(dev, dr);
  return 0;
}

EXPORT_SYMBOL(devm_request_irq);
