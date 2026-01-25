#pragma once

#include <aerosync/types.h>

/*
 * AeroSync Memory Map (x86_64)
 *
 * Virtual Address Space Layout (Canonical Higher Half):
 *
 * +----------------------+ 0xFFFFFFFFFFFFFFFF
 * |                      |
 * |    Kernel Text/Data  | (~2GB, fixed at compile time)
 * |  0xFFFFFFFF80000000  |
 * +----------------------+
 * |                      |
 * |        Unused        | (Gap)
 * |                      |
 * +----------------------+ 0xFFFF901000000000
 * |                      |
 * |       Vmalloc        | (64GB - For vmalloc/viomap)
 * |  0xFFFF900000000000  |
 * +----------------------+ 0xFFFF900000000000
 * |                      |
 * |         HHDM         | (Direct Map of Physical RAM)
 * |  (Slab lives here)   | (Max 16TB supported range)
 * |  0xFFFF800000000000  |
 * +----------------------+ 0xFFFF800000000000
 */

/*
 * 1. Kernel Image
 * Located in the top 2GB of address space.
 */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_VIRT_END  0xFFFFFFFFFFFFFFFFULL
#define KERNEL_VIRT_SIZE (KERNEL_VIRT_END - KERNEL_VIRT_BASE + 1ULL)

/*
 * 2. HHDM (Higher Half Direct Map) & Slab
 * This is where Limine maps all physical memory.
 * The Slab allocator (kmalloc) returns addresses in this range.
 * Range: 0xFFFF800000000000 -> 0xFFFF900000000000 (16 TB space)
 */
#define HHDM_VIRT_BASE   0xFFFF800000000000ULL
#define HHDM_VIRT_LIMIT  0xFFFF900000000000ULL /* Soft limit for safety */
#define HHDM_VIRT_SIZE   (HHDM_VIRT_LIMIT - HHDM_VIRT_BASE)

#define SLAB_VIRT_BASE   HHDM_VIRT_BASE
#define SLAB_VIRT_END    HHDM_VIRT_LIMIT
#define SLAB_VIRT_SIZE   HHDM_VIRT_SIZE

/*
 * 3. Vmalloc Allocator Region
 * Used by vmalloc() and viomap() for non-contiguous or IO mappings.
 * Starts at 16TB offset, ensuring it never overlaps HHDM.
 * Size: Configurable via Kconfig (default 64GB)
 */
#ifndef CONFIG_VMALLOC_SIZE_GB
#define CONFIG_VMALLOC_SIZE_GB 64
#endif

#define VMALLOC_VIRT_BASE 0xFFFF900000000000ULL
#define VMALLOC_VIRT_SIZE ((uint64_t)CONFIG_VMALLOC_SIZE_GB * 1024 * 1024 * 1024)
#define VMALLOC_VIRT_END  (VMALLOC_VIRT_BASE + VMALLOC_VIRT_SIZE)

/* Helper to check if address is in kernel high memory */
static inline bool is_kernel_addr(uint64_t addr) {
  return addr >= HHDM_VIRT_BASE;
}