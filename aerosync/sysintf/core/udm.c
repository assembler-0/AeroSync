/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/core/udm.c
 * @brief Unified Driver Management - Core orchestration
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
#include <mm/slub.h>

struct udm_device_entry {
  struct device *dev;
  const struct udm_ops *ops;
  enum udm_driver_state state;
  struct list_head node;
};

static LIST_HEAD(udm_device_list);
static mutex_t udm_lock;
static enum udm_state global_state = UDM_STATE_RUNNING;

void udm_init(void) {
  mutex_init(&udm_lock);
  printk(KERN_INFO HAL_CLASS "Unified Driver Management initialized\n");
}

int udm_register_ops(struct device *dev, const struct udm_ops *ops) {
  if (!dev || !ops) return -EINVAL;

  struct udm_device_entry *entry = kmalloc(sizeof(*entry));
  if (!entry) return -ENOMEM;

  entry->dev = get_device(dev);
  entry->ops = ops;
  entry->state = UDM_DRIVER_ACTIVE;

  mutex_lock(&udm_lock);
  list_add_tail(&entry->node, &udm_device_list);
  mutex_unlock(&udm_lock);

#ifdef CONFIG_DEBUG_UDM
  printk(KERN_DEBUG HAL_CLASS "Registered UDM ops for %s\n", dev->name);
#endif

  return 0;
}
EXPORT_SYMBOL(udm_register_ops);

enum udm_state udm_get_state(void) {
  return global_state;
}
EXPORT_SYMBOL(udm_get_state);

enum udm_driver_state udm_get_driver_state(struct device *dev) {
  struct udm_device_entry *entry;
  enum udm_driver_state state = UDM_DRIVER_ACTIVE;

  mutex_lock(&udm_lock);
  list_for_each_entry(entry, &udm_device_list, node) {
    if (entry->dev == dev) {
      state = entry->state;
      break;
    }
  }
  mutex_unlock(&udm_lock);

  return state;
}
EXPORT_SYMBOL(udm_get_driver_state);

int udm_suspend_device(struct device *dev) {
  struct udm_device_entry *entry;
  int ret = -ENODEV;

  mutex_lock(&udm_lock);
  list_for_each_entry(entry, &udm_device_list, node) {
    if (entry->dev == dev) {
      if (entry->ops->suspend) {
        ret = entry->ops->suspend(dev);
        if (ret == 0) entry->state = UDM_DRIVER_SUSPENDED;
        else entry->state = UDM_DRIVER_ERROR;
      } else {
        ret = 0;
      }
      break;
    }
  }
  mutex_unlock(&udm_lock);

  return ret;
}
EXPORT_SYMBOL(udm_suspend_device);

int udm_resume_device(struct device *dev) {
  struct udm_device_entry *entry;
  int ret = -ENODEV;

  mutex_lock(&udm_lock);
  list_for_each_entry(entry, &udm_device_list, node) {
    if (entry->dev == dev) {
      if (entry->ops->resume) {
        ret = entry->ops->resume(dev);
        if (ret == 0) entry->state = UDM_DRIVER_ACTIVE;
        else entry->state = UDM_DRIVER_ERROR;
      } else {
        ret = 0;
      }
      break;
    }
  }
  mutex_unlock(&udm_lock);

  return ret;
}
EXPORT_SYMBOL(udm_resume_device);

int udm_suspend_all(void) {
  struct udm_device_entry *entry;
  int ret = 0, failed = 0;

  printk(KERN_INFO HAL_CLASS "Suspending all drivers...\n");

  mutex_lock(&udm_lock);
  global_state = UDM_STATE_SUSPENDING;

  list_for_each_entry_reverse(entry, &udm_device_list, node) {
    if (entry->state != UDM_DRIVER_ACTIVE) continue;

    if (entry->ops->suspend) {
      int r = entry->ops->suspend(entry->dev);
      if (r != 0) {
        printk(KERN_ERR HAL_CLASS "Failed to suspend %s: %d\n", 
               entry->dev->name, r);
        entry->state = UDM_DRIVER_ERROR;
        failed++;
        ret = r;
      } else {
        entry->state = UDM_DRIVER_SUSPENDED;
#ifdef CONFIG_DEBUG_UDM
        printk(KERN_DEBUG HAL_CLASS "Suspended %s\n", entry->dev->name);
#endif
      }
    }
  }

  if (failed == 0) {
    global_state = UDM_STATE_SUSPENDED;
    printk(KERN_INFO HAL_CLASS "All drivers suspended\n");
  } else {
    global_state = UDM_STATE_RUNNING;
    printk(KERN_ERR HAL_CLASS "Suspend failed (%d drivers)\n", failed);
  }

  mutex_unlock(&udm_lock);
  return ret;
}
EXPORT_SYMBOL(udm_suspend_all);

int udm_resume_all(void) {
  struct udm_device_entry *entry;
  int ret = 0, failed = 0;

  printk(KERN_INFO HAL_CLASS "Resuming all drivers...\n");

  mutex_lock(&udm_lock);
  global_state = UDM_STATE_RESUMING;

  list_for_each_entry(entry, &udm_device_list, node) {
    if (entry->state != UDM_DRIVER_SUSPENDED) continue;

    if (entry->ops->resume) {
      int r = entry->ops->resume(entry->dev);
      if (r != 0) {
        printk(KERN_ERR HAL_CLASS "Failed to resume %s: %d\n",
               entry->dev->name, r);
        entry->state = UDM_DRIVER_ERROR;
        failed++;
        ret = r;
      } else {
        entry->state = UDM_DRIVER_ACTIVE;
#ifdef CONFIG_DEBUG_UDM
        printk(KERN_DEBUG HAL_CLASS "Resumed %s\n", entry->dev->name);
#endif
      }
    }
  }

  global_state = UDM_STATE_RUNNING;
  if (failed > 0) {
    printk(KERN_ERR HAL_CLASS "Resume completed with %d errors\n", failed);
  } else {
    printk(KERN_INFO HAL_CLASS "All drivers resumed\n");
  }

  mutex_unlock(&udm_lock);
  return ret;
}
EXPORT_SYMBOL(udm_resume_all);

int udm_stop_all(void) {
  struct udm_device_entry *entry;
  int ret = 0, failed = 0;

  printk(KERN_INFO HAL_CLASS "Stopping all drivers...\n");

  mutex_lock(&udm_lock);
  global_state = UDM_STATE_SHUTTING_DOWN;

  list_for_each_entry_reverse(entry, &udm_device_list, node) {
    if (entry->state == UDM_DRIVER_STOPPED) continue;

    if (entry->ops->stop) {
      int r = entry->ops->stop(entry->dev);
      if (r != 0) {
        printk(KERN_ERR HAL_CLASS "Failed to stop %s: %d\n",
               entry->dev->name, r);
        entry->state = UDM_DRIVER_ERROR;
        failed++;
        ret = r;
      } else {
        entry->state = UDM_DRIVER_STOPPED;
#ifdef CONFIG_DEBUG_UDM
        printk(KERN_DEBUG HAL_CLASS "Stopped %s\n", entry->dev->name);
#endif
      }
    }
  }

  if (failed == 0) {
    printk(KERN_INFO HAL_CLASS "All drivers stopped\n");
  } else {
    printk(KERN_ERR HAL_CLASS "Stop failed (%d drivers)\n", failed);
  }

  mutex_unlock(&udm_lock);
  return ret;
}
EXPORT_SYMBOL(udm_stop_all);

int udm_restart_all(void) {
  struct udm_device_entry *entry;
  int ret = 0, failed = 0;

  printk(KERN_INFO HAL_CLASS "Restarting all drivers...\n");

  mutex_lock(&udm_lock);

  list_for_each_entry(entry, &udm_device_list, node) {
    if (entry->state != UDM_DRIVER_STOPPED) continue;

    if (entry->ops->restart) {
      int r = entry->ops->restart(entry->dev);
      if (r != 0) {
        printk(KERN_ERR HAL_CLASS "Failed to restart %s: %d\n",
               entry->dev->name, r);
        entry->state = UDM_DRIVER_ERROR;
        failed++;
        ret = r;
      } else {
        entry->state = UDM_DRIVER_ACTIVE;
#ifdef CONFIG_DEBUG_UDM
        printk(KERN_DEBUG HAL_CLASS "Restarted %s\n", entry->dev->name);
#endif
      }
    }
  }

  global_state = UDM_STATE_RUNNING;
  if (failed > 0) {
    printk(KERN_ERR HAL_CLASS "Restart completed with %d errors\n", failed);
  } else {
    printk(KERN_INFO HAL_CLASS "All drivers restarted\n");
  }

  mutex_unlock(&udm_lock);
  return ret;
}
EXPORT_SYMBOL(udm_restart_all);

void udm_shutdown_all(void) {
  struct udm_device_entry *entry;

  printk(KERN_INFO HAL_CLASS "Shutting down all drivers...\n");

  mutex_lock(&udm_lock);
  global_state = UDM_STATE_SHUTTING_DOWN;

  list_for_each_entry_reverse(entry, &udm_device_list, node) {
    if (entry->dev->driver && entry->dev->driver->shutdown) {
      entry->dev->driver->shutdown(entry->dev);
#ifdef CONFIG_DEBUG_UDM
      printk(KERN_DEBUG HAL_CLASS "Shutdown %s\n", entry->dev->name);
#endif
    }
  }

  global_state = UDM_STATE_HALTED;
  printk(KERN_INFO HAL_CLASS "All drivers shut down\n");

  mutex_unlock(&udm_lock);
}
EXPORT_SYMBOL(udm_shutdown_all);

void udm_emergency_stop_all(void) {
  struct udm_device_entry *entry;

  printk(KERN_EMERG HAL_CLASS "EMERGENCY STOP - Halting all drivers\n");

  list_for_each_entry_reverse(entry, &udm_device_list, node) {
    if (entry->ops->emergency_stop) {
      entry->ops->emergency_stop(entry->dev);
    }
  }

  global_state = UDM_STATE_HALTED;
}
EXPORT_SYMBOL(udm_emergency_stop_all);
