#pragma once

#include <kernel/types.h>

/*
 * Allocates a contiguous region of virtual memory in the kernel execution
 * environment, backed by (potentially non-contiguous) physical pages.
 *
 * Content is undefined.
 */
void *vmalloc(size_t size);

/*
 * Same as vmalloc, but zeroes the memory.
 */
void *vzalloc(size_t size);

/*
 * Allocates executable memory (e.g., for loading kernel modules).
 */
void *vmalloc_exec(size_t size);

/*
 * Frees memory allocated by vmalloc/vzalloc/vmalloc_exec.
 */
void vfree(void *addr);

/*
 * Maps a specific physical address range to the vmalloc region.
 * Useful for mapping MMIO/Framebuffer regions.
 */
void *viomap(uint64_t phys_addr, size_t size);

/*
 * Maps a specific physical address range using Write-Combining (WC).
 * Ideal for framebuffers.
 */
void *viomap_wc(uint64_t phys_addr, size_t size);

/*
 * Unmaps an IO mapping.
 */
void viounmap(void *addr);

/*
 * Do a stress test, panics if failed
 */
void vmalloc_test(void);
