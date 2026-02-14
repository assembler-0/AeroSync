/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/iommu.h
 * @brief Generic IOMMU Abstraction System
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <linux/list.h>

struct device;
struct iommu_domain;
struct dma_map_ops;
struct page;

enum iommu_cap {
  IOMMU_CAP_CACHE_COHERENCY, /* IOMMU can enforce cache coherency */
  IOMMU_CAP_INTR_REMAP,      /* IOMMU supports interrupt remapping */
};

struct iommu_ops {
  int (*domain_init)(struct iommu_domain *domain);
  void (*domain_free)(struct iommu_domain *domain);
  int (*attach_dev)(struct iommu_domain *domain, struct device *dev);
  void (*detach_dev)(struct iommu_domain *domain, struct device *dev);
  int (*map)(struct iommu_domain *domain, uint64_t iova, uint64_t paddr, size_t size, int prot);
  size_t (*unmap)(struct iommu_domain *domain, uint64_t iova, size_t size);
  uint64_t (*iova_to_phys)(struct iommu_domain *domain, uint64_t iova);
  bool (*capable)(enum iommu_cap cap);
};

struct iommu_domain {
  const struct iommu_ops *ops;
  void *priv; /* Driver private data */
  uint64_t pgtable; /* Root of page table */
};

/**
 * @brief Register an IOMMU hardware unit with the system
 */
int iommu_register_ops(const struct iommu_ops *ops, const struct dma_map_ops *dma_ops);

/**
 * @brief Associate a device with its corresponding IOMMU
 */
int iommu_probe_device(struct device *dev);

/**
 * @brief Release IOMMU resources for a device
 */
void iommu_release_device(struct device *dev);

/**
 * @brief High-level IOMMU Domain API
 */
struct iommu_domain *iommu_domain_alloc(void);
void iommu_domain_free(struct iommu_domain *domain);
int iommu_attach_device(struct iommu_domain *domain, struct device *dev);
void iommu_detach_device(struct iommu_domain *domain, struct device *dev);
int iommu_map(struct iommu_domain *domain, uint64_t iova, uint64_t paddr, size_t size, int prot);
size_t iommu_unmap(struct iommu_domain *domain, uint64_t iova, size_t size);
