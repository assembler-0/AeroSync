/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/iommu.c
 * @brief Generic IOMMU Abstraction System Implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/iommu.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <aerosync/errno.h>
#include <lib/string.h>

#include <aerosync/export.h>

static const struct iommu_ops *s_registered_ops = nullptr;
static const struct dma_map_ops *s_iommu_dma_ops = nullptr;

int iommu_register_ops(const struct iommu_ops *ops, const struct dma_map_ops *dma_ops) {
  if (s_registered_ops) {
    printk(KERN_WARNING IOMMU_CLASS "IOMMU ops already registered\n");
    return -EBUSY;
  }
  s_registered_ops = ops;
  s_iommu_dma_ops = dma_ops;
  printk(KERN_INFO IOMMU_CLASS "Generic IOMMU abstraction layer initialized\n");
  return 0;
}
EXPORT_SYMBOL(iommu_register_ops);

int iommu_probe_device(struct device *dev) {
  if (!s_registered_ops) return -ENODEV;
  
  /* If the IOMMU driver is active, attach its DMA ops to the device */
  if (s_iommu_dma_ops) {
    dev->dma_ops = (struct dma_map_ops *)s_iommu_dma_ops;
  }

  return 0; 
}
EXPORT_SYMBOL(iommu_probe_device);

void iommu_release_device(struct device *dev) {
  (void)dev;
}
EXPORT_SYMBOL(iommu_release_device);

struct iommu_domain *iommu_domain_alloc(void) {
  if (!s_registered_ops) return nullptr;

  auto domain = (struct iommu_domain *)kmalloc(sizeof(struct iommu_domain));
  if (!domain) return nullptr;

  memset(domain, 0, sizeof(*domain));
  domain->ops = s_registered_ops;

  if (domain->ops->domain_init && domain->ops->domain_init(domain) != 0) {
    kfree(domain);
    return nullptr;
  }

  return domain;
}
EXPORT_SYMBOL(iommu_domain_alloc);

void iommu_domain_free(struct iommu_domain *domain) {
  if (!domain) return;
  if (domain->ops->domain_free)
    domain->ops->domain_free(domain);
  kfree(domain);
}
EXPORT_SYMBOL(iommu_domain_free);

int iommu_attach_device(struct iommu_domain *domain, struct device *dev) {
  if (!domain || !dev || !domain->ops->attach_dev) return -EINVAL;
  return domain->ops->attach_dev(domain, dev);
}
EXPORT_SYMBOL(iommu_attach_device);

void iommu_detach_device(struct iommu_domain *domain, struct device *dev) {
  if (!domain || !dev || !domain->ops->detach_dev) return;
  domain->ops->detach_dev(domain, dev);
}
EXPORT_SYMBOL(iommu_detach_device);

int iommu_map(struct iommu_domain *domain, uint64_t iova, uint64_t paddr, size_t size, int prot) {
  if (!domain || !domain->ops->map) return -EINVAL;
  return domain->ops->map(domain, iova, paddr, size, prot);
}
EXPORT_SYMBOL(iommu_map);

size_t iommu_unmap(struct iommu_domain *domain, uint64_t iova, size_t size) {
  if (!domain || !domain->ops->unmap) return 0;
  return domain->ops->unmap(domain, iova, size);
}
EXPORT_SYMBOL(iommu_unmap);
