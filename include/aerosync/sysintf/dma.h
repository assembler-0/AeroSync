/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/dma.h
 * @brief DMA System Interface
 * @copyright (C) 2025 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <mm/gfp.h>

typedef uint64_t dma_addr_t;

enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

/**
 * Allocate physically contiguous, DMA-capable memory
 * @param size Size in bytes
 * @param dma_handle Output: Physical address for the device
 * @param gfp Flags (e.g., GFP_KERNEL)
 * @return Virtual address for the CPU
 */
void *dma_alloc_coherent(size_t size, dma_addr_t *dma_handle, gfp_t gfp);

/**
 * Free memory allocated via dma_alloc_coherent
 */
void dma_free_coherent(size_t size, void *cpu_addr, dma_addr_t dma_handle);

/**
 * Map a virtual buffer for DMA access
 */
dma_addr_t dma_map_single(void *ptr, size_t size, enum dma_data_direction dir);

/**
 * Unmap a buffer mapped via dma_map_single
 */
void dma_unmap_single(dma_addr_t dma_addr, size_t size, enum dma_data_direction dir);
