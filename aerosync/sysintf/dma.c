/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/dma.c
 * @brief Direct Memory Access engine
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/dma.h>
#include <aerosync/sysintf/device.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/printk.h>
#include <mm/gfp.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/classes.h>

static inline const struct dma_map_ops *get_dma_ops(struct device *dev) {
  if (dev && dev->dma_ops)
    return dev->dma_ops;
  return &direct_dma_ops;
}

void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp) {
  const struct dma_map_ops *ops = get_dma_ops(dev);
  if (ops->alloc)
    return ops->alloc(dev, size, dma_handle, gfp);
  return nullptr;
}
EXPORT_SYMBOL(dma_alloc_coherent);

void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle) {
  const struct dma_map_ops *ops = get_dma_ops(dev);
  if (ops->free)
    ops->free(dev, size, cpu_addr, dma_handle);
}
EXPORT_SYMBOL(dma_free_coherent);

dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size, enum dma_data_direction dir) {
  const struct dma_map_ops *ops = get_dma_ops(dev);
  struct page *page = virt_to_page(ptr);
  unsigned long offset = (unsigned long)ptr & ~PAGE_MASK;

  if (ops->map_page)
    return ops->map_page(dev, page, offset, size, dir);
  return (dma_addr_t)-1;
}
EXPORT_SYMBOL(dma_map_single);

void dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size, enum dma_data_direction dir) {
  const struct dma_map_ops *ops = get_dma_ops(dev);
  if (ops->unmap_page)
    ops->unmap_page(dev, dma_addr, size, dir);
}
EXPORT_SYMBOL(dma_unmap_single);

/* Direct DMA implementation (no IOMMU) */

static void *direct_alloc(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp) {
  (void)dev;
  struct folio *folio;
  if (!(gfp & (GFP_DMA | GFP_DMA32))) {
    gfp |= GFP_DMA32;
  }

  size_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint32_t order = 0;
  while ((1u << order) < count) order++;

  if (order >= MAX_ORDER) {
    printk(KERN_ERR DMA_CLASS "Requested size %zu too large (order %u)\n", size, order);
    return nullptr;
  }

  folio = alloc_pages(gfp | ___GFP_ZERO, order);
  if (!folio) return nullptr;

  uint64_t phys = folio_to_phys(folio);
  *dma_handle = (dma_addr_t)phys;

  return page_address(&folio->page);
}

static void direct_free(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle) {
  (void)dev; (void)cpu_addr;
  size_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
  uint32_t order = 0;
  while ((1u << order) < count) order++;

  struct page *page = phys_to_page(dma_handle);
  __free_pages(page, order);
}

static dma_addr_t direct_map_page(struct device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction dir) {
  (void)dev; (void)size; (void)dir;
  return (dma_addr_t)page_to_phys(page) + offset;
}

static void direct_unmap_page(struct device *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir) {
  (void)dev; (void)dma_handle; (void)size; (void)dir;
}

const struct dma_map_ops direct_dma_ops = {
  .alloc = direct_alloc,
  .free = direct_free,
  .map_page = direct_map_page,
  .unmap_page = direct_unmap_page,
};
EXPORT_SYMBOL(direct_dma_ops);