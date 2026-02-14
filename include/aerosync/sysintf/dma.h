/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/dma.h
 * @brief DMA System Interface
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <mm/gfp.h>
#include <mm/page.h>

typedef uint64_t dma_addr_t;

struct device;

enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

struct dma_map_ops {
    void *(*alloc)(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp);
    void (*free)(struct device *dev, size_t size, void *vaddr, dma_addr_t dma_handle);
    dma_addr_t (*map_page)(struct device *dev, struct page *page, unsigned long offset, size_t size, enum dma_data_direction dir);
    void (*unmap_page)(struct device *dev, dma_addr_t dma_handle, size_t size, enum dma_data_direction dir);
};

/**
 * Allocate physically contiguous, DMA-capable memory
 * @param dev Device for which to allocate
 * @param size Size in bytes
 * @param dma_handle Output: Physical or I/O address for the device
 * @param gfp Flags (e.g., GFP_KERNEL)
 * @return Virtual address for the CPU
 */
void *dma_alloc_coherent(struct device *dev, size_t size, dma_addr_t *dma_handle, gfp_t gfp);

/**
 * Free memory allocated via dma_alloc_coherent
 */
void dma_free_coherent(struct device *dev, size_t size, void *cpu_addr, dma_addr_t dma_handle);

/**
 * Map a virtual buffer for DMA access
 */
dma_addr_t dma_map_single(struct device *dev, void *ptr, size_t size, enum dma_data_direction dir);

/**
 * Unmap a buffer mapped via dma_map_single
 */
void dma_unmap_single(struct device *dev, dma_addr_t dma_addr, size_t size, enum dma_data_direction dir);

/* Fallback ops for when no IOMMU is present */
extern const struct dma_map_ops direct_dma_ops;
