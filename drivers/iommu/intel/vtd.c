/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/iommu/intel/vtd.c
 * @brief Intel VT-d IOMMU FKX Module
 * @copyright (C) 2025-2026 assembler-0
 */

#include <drivers/iommu/intel-iommu.h>
#include <aerosync/sysintf/dmar.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/sysintf/iommu.h>
#include <aerosync/sysintf/pci.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <arch/x86_64/cpu.h>
#include <lib/printk.h>
#include <mm/vmalloc.h>
#include <mm/slub.h>
#include <mm/vma.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/string.h>

static LIST_HEAD(s_iommus);

static inline unsigned int vtd_get_order(size_t size) {
  unsigned int order = 0;
  size = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
  while (size > 1) {
    size >>= 1;
    order++;
  }
  return order;
}

static inline void vtd_write32(struct intel_iommu *iommu, uint32_t reg, uint32_t val) {
  *(volatile uint32_t *)((uintptr_t)iommu->reg_virt + reg) = val;
}

static inline uint32_t vtd_read32(struct intel_iommu *iommu, uint32_t reg) {
  return *(volatile uint32_t *)((uintptr_t)iommu->reg_virt + reg);
}

static inline uint64_t vtd_read64(struct intel_iommu *iommu, uint32_t reg) {
  return *(volatile uint64_t *)((uintptr_t)iommu->reg_virt + reg);
}

struct intel_iommu *find_iommu_for_device(uint16_t segment, uint8_t bus, uint8_t devfn) {
  struct intel_iommu *iommu;
  list_for_each_entry(iommu, &s_iommus, node) {
    if (iommu->segment == segment) {
       return iommu;
    }
  }
  return nullptr;
}

/* --- DMA Ops implementation --- */

static void *vtd_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp) {
  auto folio = alloc_pages(gfp | ___GFP_ZERO, vtd_get_order(size));
  if (!folio) return nullptr;

  *dma_handle = folio_to_phys(folio);
  return page_address(&folio->page);
}

static void vtd_free_coherent(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle) {
  (void)dev;
  __free_pages(phys_to_page((uintptr_t)cpu_addr), vtd_get_order(size));
}

static dma_addr_t vtd_map_page(struct device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction dir) {
  (void)dev; (void)size; (void)dir;
  return page_to_phys(page) + offset;
}

static void vtd_unmap_page(struct device *dev, dma_addr_t dma_addr, size_t size, enum dma_data_direction dir) {
  (void)dev; (void)dma_addr; (void)size; (void)dir;
}

static const struct dma_map_ops vtd_dma_ops = {
  .alloc = vtd_alloc_coherent,
  .free = vtd_free_coherent,
  .map_page = vtd_map_page,
  .unmap_page = vtd_unmap_page,
};

/* --- IOMMU Ops implementation --- */

static int vtd_domain_init(struct iommu_domain *domain) {
  auto folio = alloc_pages(GFP_KERNEL | ___GFP_ZERO, 0);
  if (!folio) return -ENOMEM;

  domain->pgtable = (uintptr_t)page_address(&folio->page);
  return 0;
}

static void vtd_domain_free(struct iommu_domain *domain) {
  if (domain->pgtable) {
    __free_pages(phys_to_page(domain->pgtable), 0);
  }
}

static int vtd_attach_dev(struct iommu_domain *domain, struct device *dev) {
  (void)domain; (void)dev;
  return 0;
}

static int vtd_map(struct iommu_domain *domain, uint64_t iova, uint64_t paddr, size_t size, int prot) {
  (void)domain; (void)iova; (void)paddr; (void)size; (void)prot;
  return 0;
}

static const struct iommu_ops vtd_iommu_ops = {
  .domain_init = vtd_domain_init,
  .domain_free = vtd_domain_free,
  .attach_dev = vtd_attach_dev,
  .map = vtd_map,
};

/**
 * @brief Initialize a single IOMMU hardware unit
 */
static int iommu_init_unit(struct intel_iommu *iommu) {
  iommu->reg_virt = ioremap(iommu->reg_phys, PAGE_SIZE);
  if (!iommu->reg_virt) return -ENOMEM;

  iommu->cap = vtd_read64(iommu, DMAR_CAP_REG);
  iommu->ecap = vtd_read64(iommu, DMAR_ECAP_REG);

  auto folio = alloc_pages(GFP_KERNEL | ___GFP_ZERO, 0);
  if (!folio) return -ENOMEM;

  iommu->root_entry = (struct root_entry *)page_address(&folio->page);
  auto root_phys = folio_to_phys(folio);

  vtd_write32(iommu, DMAR_RTADDR_REG, (uint32_t)root_phys);
  vtd_write32(iommu, DMAR_RTADDR_REG + 4, (uint32_t)(root_phys >> 32));
  vtd_write32(iommu, DMAR_GCMD_REG, DMAR_GCMD_SRTP);

  while (!(vtd_read32(iommu, DMAR_GSTS_REG) & DMAR_GSTS_RTPS)) cpu_relax();

  vtd_write32(iommu, DMAR_GCMD_REG, DMAR_GCMD_TE);
  while (!(vtd_read32(iommu, DMAR_GSTS_REG) & DMAR_GSTS_TES)) cpu_relax();

  return 0;
}

static int vtd_mod_init(void) {
  auto units = dmar_get_units();
  if (list_empty(units)) {
    printk(KERN_ERR IOMMU_CLASS "No Intel VT-d units found\n");
    return -ENODEV;
  }

  dmar_unit_t *dmar_unit;
  list_for_each_entry(dmar_unit, units, node) {
    auto iommu = (struct intel_iommu *)kmalloc(sizeof(struct intel_iommu));
    if (!iommu) continue;

    memset(iommu, 0, sizeof(*iommu));
    iommu->reg_phys = dmar_unit->address;
    iommu->segment = dmar_unit->segment;
    spinlock_init(&iommu->lock);

    if (iommu_init_unit(iommu) == 0) {
      list_add_tail(&iommu->node, &s_iommus);
      printk(KERN_INFO IOMMU_CLASS "Intel VT-d Unit @ 0x%llx initialized\n", iommu->reg_phys);
    } else {
      kfree(iommu);
    }
  }

  if (!list_empty(&s_iommus)) {
    iommu_register_ops(&vtd_iommu_ops, &vtd_dma_ops);
  }

  return list_empty(&s_iommus) ? -ENODEV : 0;
}

FKX_MODULE_DEFINE(
  vtd,
  "1.0.0",
  "assembler-0",
  "Intel VT-d IOMMU Driver",
  0,
  FKX_DRIVER_CLASS,
  vtd_mod_init,
  nullptr
);
