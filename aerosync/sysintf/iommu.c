/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/iommu.c
 * @brief Generic IOMMU Abstraction System (Unified Model)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/iommu.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/rcu.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>

struct iommu_device {
  struct device dev;
};

static int iommu_evaluate(struct device *a, struct device *b) {
  const struct iommu_ops *ops_a = a->ops;
  const struct iommu_ops *ops_b = b->ops;
  /* Higher priority IOMMU wins (e.g. VT-d over GART) */
  return (int)(ops_a->priority - ops_b->priority);
}

struct class iommu_class = {
  .name = "iommu",
  .is_singleton = true,
  .evaluate = iommu_evaluate,
};

static bool iommu_class_registered = false;

int iommu_register_ops(const struct iommu_ops *ops, const struct dma_map_ops *dma_ops) {
  if (unlikely(!iommu_class_registered)) {
    class_register(&iommu_class);
    iommu_class_registered = true;
  }

  struct iommu_device *idev = kzalloc(sizeof(struct iommu_device));
  if (!idev) return -ENOMEM;

  /* We store both IOMMU and DMA ops in the device's ops field. 
   * Since iommu_ops is the primary interface, we use it as the main ops.
   */
  idev->dev.ops = (void *)ops;
  idev->dev.driver_data = (void *)dma_ops; /* Use driver_data for associated DMA ops */
  idev->dev.class = &iommu_class;
  idev->dev.name = "iommu";

  if (device_register(&idev->dev) != 0) {
    kfree(idev);
    return -EFAULT;
  }

  printk(KERN_INFO IOMMU_CLASS "Registered IOMMU: %s\n", ops->name);
  return 0;
}
EXPORT_SYMBOL(iommu_register_ops);

#define IOMMU_ACTIVE_OPS() ((struct iommu_ops *)class_get_active_interface(&iommu_class))

int iommu_probe_device(struct device *dev) {
  rcu_read_lock();
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  if (ops) {
    /* Get the device that provides these ops to find its associated DMA ops */
    struct device *iommu_dev = iommu_class.active_dev;
    if (iommu_dev) {
      dev->dma_ops = (struct dma_map_ops *)iommu_dev->driver_data;
    }
  }
  rcu_read_unlock();
  return 0; 
}
EXPORT_SYMBOL(iommu_probe_device);

struct iommu_domain *iommu_domain_alloc(void) {
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  if (!ops) return nullptr;

  struct iommu_domain *domain = kzalloc(sizeof(struct iommu_domain));
  if (!domain) return nullptr;

  domain->ops = ops;
  if (domain->ops->domain_init && domain->ops->domain_init(domain) != 0) {
    kfree(domain);
    return nullptr;
  }

  return domain;
}
EXPORT_SYMBOL(iommu_domain_alloc);

/* Dispatchers using IOMMU_ACTIVE_OPS() */

int iommu_attach_device(struct iommu_domain *domain, struct device *dev) {
  if (!domain || !dev) return -EINVAL;
  rcu_read_lock();
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  int ret = (ops && ops->attach_dev) ? ops->attach_dev(domain, dev) : -ENOSYS;
  rcu_read_unlock();
  return ret;
}
EXPORT_SYMBOL(iommu_attach_device);

void iommu_detach_device(struct iommu_domain *domain, struct device *dev) {
  if (!domain || !dev) return;
  rcu_read_lock();
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  if (ops && ops->detach_dev) ops->detach_dev(domain, dev);
  rcu_read_unlock();
}
EXPORT_SYMBOL(iommu_detach_device);

int iommu_map(struct iommu_domain *domain, uint64_t iova, uint64_t paddr, size_t size, int prot) {
  if (!domain) return -EINVAL;
  rcu_read_lock();
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  int ret = (ops && ops->map) ? ops->map(domain, iova, paddr, size, prot) : -ENOSYS;
  rcu_read_unlock();
  return ret;
}
EXPORT_SYMBOL(iommu_map);

size_t iommu_unmap(struct iommu_domain *domain, uint64_t iova, size_t size) {
  if (!domain) return 0;
  rcu_read_lock();
  struct iommu_ops *ops = IOMMU_ACTIVE_OPS();
  size_t ret = (ops && ops->unmap) ? ops->unmap(domain, iova, size) : 0;
  rcu_read_unlock();
  return ret;
}
EXPORT_SYMBOL(iommu_unmap);
