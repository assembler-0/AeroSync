/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file kernel/sysintf/dma.c
 * @brief DMA System Interface Implementation
 * @copyright (C) 2025 assembler-0
 */

#include <kernel/sysintf/dma.h>
#include <arch/x64/mm/pmm.h>
#include <lib/printk.h>
#include <mm/gfp.h>
#include <kernel/fkx/fkx.h>

void *dma_alloc_coherent(size_t size, dma_addr_t *dma_handle, gfp_t gfp) {
    if (!size || !dma_handle) return NULL;

    // Use GFP_DMA32 by default if not specified, to ensure 32-bit compatibility
    if (!(gfp & (GFP_DMA | GFP_DMA32))) {
        gfp |= GFP_DMA32;
    }

    size_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    struct page *page = alloc_pages(gfp, 0); // TODO: Implement order allocation correctly if needed
    
    // Simple implementation for now: only single page or order-based allocation
    // For contiguous pages, we need to pass the correct order.
    
    uint32_t order = 0;
    while ((1u << order) < count) order++;

    if (order >= MAX_ORDER) {
        printk(KERN_ERR "DMA: Requested size %zu too large (order %u)\n", size, order);
        return NULL;
    }

    page = alloc_pages(gfp, order);
    if (!page) return NULL;

    uint64_t phys = PFN_TO_PHYS(page_to_pfn(page));
    *dma_handle = (dma_addr_t)phys;

    void *virt = page_address(page);
    
    // Coherent memory should be zeroed
    if (gfp & ___GFP_ZERO) {
        // memset is handled by GFP_ZERO usually, but let's be safe
        // (Implementation dependent)
    }

    return virt;
}
EXPORT_SYMBOL(dma_alloc_coherent);

void dma_free_coherent(size_t size, void *cpu_addr, dma_addr_t dma_handle) {
    if (!cpu_addr) return;

    size_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t order = 0;
    while ((1u << order) < count) order++;

    struct page *page = phys_to_page(dma_handle);
    __free_pages(page, order);
}
EXPORT_SYMBOL(dma_free_coherent);

dma_addr_t dma_map_single(void *ptr, size_t size, enum dma_data_direction dir) {
    (void)size;
    (void)dir;
    // Simple implementation: assume physical memory is identity mapped/traceable
    return (dma_addr_t)pmm_virt_to_phys(ptr);
}
EXPORT_SYMBOL(dma_map_single);

void dma_unmap_single(dma_addr_t dma_addr, size_t size, enum dma_data_direction dir) {
    (void)dma_addr;
    (void)size;
    (void)dir;
    // Nothing to do for simple physical mapping
}
EXPORT_SYMBOL(dma_unmap_single);
