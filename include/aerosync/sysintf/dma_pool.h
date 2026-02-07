/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/dma_pool.h
 * @brief DMA pool allocator API
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/sysintf/dma.h>
#include <mm/gfp.h>

struct dma_pool;

/**
 * dma_pool_create - Create a DMA pool for small coherent allocations
 * @name: Pool name for debugging
 * @size: Size of each allocation in bytes
 * @align: Alignment requirement (must be power of 2)
 * @boundary: Boundary constraint (0 = no constraint)
 *
 * Creates a pool for efficient allocation of small DMA-coherent buffers.
 * Reduces fragmentation and improves performance compared to repeated
 * calls to dma_alloc_coherent().
 *
 * Returns: Pool handle or nullptr on failure
 */
struct dma_pool *dma_pool_create(const char *name, size_t size, size_t align, size_t boundary);

/**
 * dma_pool_alloc - Allocate from DMA pool
 * @pool: Pool to allocate from
 * @gfp: Allocation flags (GFP_KERNEL, GFP_ATOMIC, etc.)
 * @dma_handle: Output parameter for DMA address
 *
 * Allocates a buffer from the pool. The buffer is DMA-coherent and
 * suitable for device access.
 *
 * Returns: Virtual address or nullptr on failure
 */
void *dma_pool_alloc(struct dma_pool *pool, gfp_t gfp, dma_addr_t *dma_handle);

/**
 * dma_pool_free - Free allocation back to pool
 * @pool: Pool to free to
 * @vaddr: Virtual address returned by dma_pool_alloc()
 * @dma: DMA address returned by dma_pool_alloc()
 *
 * Returns the buffer to the pool for reuse.
 */
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma);

/**
 * dma_pool_destroy - Destroy a DMA pool
 * @pool: Pool to destroy
 *
 * Frees all resources associated with the pool. All allocations
 * must be freed before calling this function.
 */
void dma_pool_destroy(struct dma_pool *pool);
