/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/core/udm.c
 * @brief Unified Driver Management - Global Lifecycle Orchestration
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <aerosync/sysintf/udm.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/mutex.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <lib/printk.h>
#include <linux/list.h>

/* Global state tracking */
static enum udm_state global_state = UDM_STATE_RUNNING;

int udm_init(void) {
  printk(KERN_INFO HAL_CLASS "Unified Driver Management (UDM) Unification Complete\n");
  return 0;
}

enum udm_state udm_get_state(void) {
  return global_state;
}
EXPORT_SYMBOL(udm_get_state);

/* Helper to execute a lifecycle function on a device and its children */
static int udm_exec_recursive(struct device *dev, int (*func)(struct device *dev), bool reverse) {
  int ret = 0;
  struct device *child;

  if (!reverse) {
    ret = func(dev);
    if (ret) return ret;
  }

  list_for_each_entry(child, &dev->children, child_node) {
    ret = udm_exec_recursive(child, func, reverse);
    if (ret && !reverse) return ret; /* Stop on error for forward ops (suspend) */
  }

  if (reverse) {
    ret = func(dev);
  }

  return ret;
}

/* --- Lifecycle Wrappers --- */

static int dev_suspend_wrapper(struct device *dev) {
  if (dev->driver && dev->driver->suspend) {
    int r = dev->driver->suspend(dev);
    if (r == 0) {
      printk(KERN_DEBUG HAL_CLASS "Suspended %s\n", dev->name ? dev->name : "unnamed");
    }
    return r;
  }
  return 0;
}

static int dev_resume_wrapper(struct device *dev) {
  if (dev->driver && dev->driver->resume) {
    int r = dev->driver->resume(dev);
    if (r == 0) {
      printk(KERN_DEBUG HAL_CLASS "Resumed %s\n", dev->name ? dev->name : "unnamed");
    }
    return r;
  }
  return 0;
}

static int dev_shutdown_wrapper(struct device *dev) {
  if (dev->driver && dev->driver->shutdown) {
    dev->driver->shutdown(dev);
    printk(KERN_DEBUG HAL_CLASS "Shut down %s\n", dev->name ? dev->name : "unnamed");
  }
  return 0;
}

/* --- Global Commands --- */

extern struct list_head global_device_list;
extern mutex_t device_model_lock;

int udm_suspend_all(void) {
  struct device *dev;
  int ret = 0;

  printk(KERN_INFO HAL_CLASS "UDM: Suspending all system devices...\n");
  mutex_lock(&device_model_lock);
  global_state = UDM_STATE_SUSPENDING;

  /* Suspend in reverse order (peripherals before core) */
  list_for_each_entry_reverse(dev, &global_device_list, node) {
    if (!dev->parent) { /* Start from roots, udm_exec_recursive handles the rest */
      ret = udm_exec_recursive(dev, dev_suspend_wrapper, true);
      if (ret) break;
    }
  }

  global_state = (ret == 0) ? UDM_STATE_SUSPENDED : UDM_STATE_RUNNING;
  mutex_unlock(&device_model_lock);
  return ret;
}
EXPORT_SYMBOL(udm_suspend_all);

int udm_resume_all(void) {
  struct device *dev;
  int ret = 0;

  printk(KERN_INFO HAL_CLASS "UDM: Resuming all system devices...\n");
  mutex_lock(&device_model_lock);
  global_state = UDM_STATE_RESUMING;

  /* Resume in forward order (core before peripherals) */
  list_for_each_entry(dev, &global_device_list, node) {
    if (!dev->parent) {
      ret = udm_exec_recursive(dev, dev_resume_wrapper, false);
      if (ret) break;
    }
  }

  global_state = UDM_STATE_RUNNING;
  mutex_unlock(&device_model_lock);
  return ret;
}
EXPORT_SYMBOL(udm_resume_all);

void udm_shutdown_all(void) {
  struct device *dev;

  printk(KERN_INFO HAL_CLASS "UDM: Shutting down all system devices...\n");
  mutex_lock(&device_model_lock);
  global_state = UDM_STATE_SHUTTING_DOWN;

  list_for_each_entry_reverse(dev, &global_device_list, node) {
    if (!dev->parent) {
      udm_exec_recursive(dev, dev_shutdown_wrapper, true);
    }
  }

  global_state = UDM_STATE_HALTED;
  mutex_unlock(&device_model_lock);
}
EXPORT_SYMBOL(udm_shutdown_all);
