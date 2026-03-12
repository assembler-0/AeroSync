/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/graphics/drm/drm_core.c
 * @brief DRM core implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/drm/drm.h>
#include <aerosync/drm/drm_console.h>
#include <aerosync/errno.h>
#include <aerosync/sysintf/fb.h>
#include <arch/x86_64/mm/pmm.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <mm/vm_object.h>

static LIST_HEAD(drm_devices);
static struct drm_device *primary_drm = nullptr;
static spinlock_t drm_lock = SPINLOCK_INIT;

/* --- DRM Generic Device Operations --- */

static int drm_fb_mmap(struct char_device *cdev, struct vm_area_struct *vma) {
  if (!cdev || !vma) return -EINVAL;
  struct drm_device *dev = (struct drm_device *)cdev->private_data;
  if (!dev) return -ENODEV;

  size_t size = vma->vm_end - vma->vm_start;
  if (size > dev->fb.size) return -EINVAL;

  /* Use actual VRAM for userspace mmap (WC for performance) */
  uint64_t phys = pmm_virt_to_phys(dev->fb.vaddr);
  if (!phys) return -EFAULT;

  struct vm_object *obj = vm_object_device_create(phys, dev->fb.size);
  if (!obj) return -ENOMEM;

  vma->vm_obj = obj;
  vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_CACHE_WC;
  vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

  return 0;
}

static int drm_fb_ioctl(struct char_device *cdev, uint32_t cmd, void *arg) {
  struct drm_device *dev = (struct drm_device *)cdev->private_data;
  if (!dev) return -ENODEV;

  /* Basic FB IOCTLs */
  switch (cmd) {
    case 0x4600: { /* FBIOGET_VSCREENINFO equivalent */
      struct {
        uint32_t xres, yres;
        uint32_t bpp;
      } *info = (void *)arg;
      info->xres = dev->fb.width;
      info->yres = dev->fb.height;
      info->bpp = dev->fb.format.bpp;
      return 0;
    }
    case 0x4601: { /* FB_DIRTY_FLUSH */
      if (dev->driver && dev->driver->dirty_flush) {
        dev->driver->dirty_flush(dev, 0, 0, (int)dev->fb.width, (int)dev->fb.height);
        return 0;
      }
      return -ENOSYS;
    }
    default: return -EINVAL;
  }
}

static struct char_operations drm_fb_ops = {
  .mmap = drm_fb_mmap,
  .ioctl = drm_fb_ioctl,
};

/* --- Lifecycle Management --- */

int drm_dev_register(struct drm_device *dev) {
  if (!dev || !dev->driver) return -EINVAL;

  spinlock_lock(&drm_lock);
  list_add_tail(&dev->list, &drm_devices);
  if (!primary_drm) primary_drm = dev;
  spinlock_unlock(&drm_lock);

  dev->cdev = fb_register_device(&drm_fb_ops, dev);
  if (!dev->cdev) {
    return -ENOMEM;
  }

  /* Rationale: A device is now available, signal console readiness */
  drm_console_signal_ready();

  return 0;
}

void drm_dev_unregister(struct drm_device *dev) {
  if (!dev) return;

  spinlock_lock(&drm_lock);
  list_del(&dev->list);
  if (primary_drm == dev) {
    primary_drm = list_empty(&drm_devices) ? nullptr : list_first_entry(&drm_devices, struct drm_device, list);
  }
  spinlock_unlock(&drm_lock);

  if (dev->cdev) {
    fb_unregister_device(dev->cdev);
  }
}

struct drm_device *drm_get_primary(void) {
  return primary_drm;
}
