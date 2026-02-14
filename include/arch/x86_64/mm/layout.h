#pragma once

#include <aerosync/types.h>

/*
 * AeroSync Memory Map (x86_64)
 *
 * Virtual Address Space Layout (Canonical Higher Half):
 *
 * +----------------------+ 0xFFFFFFFFFFFFFFFF
 * |                      |
 * |    Kernel Text/Data  | (Dynamic Base, randomized by Limine)
 * |                      |
 * +----------------------+
 * |                      |
 * |        Unused        | (Gap)
 * |                      |
 * +----------------------+
 * |                      |
 * |       Vmalloc        | (Dynamic Base, follows HHDM)
 * |                      |
 * +----------------------+
 * |                      |
 * |         HHDM         | (Direct Map of Physical RAM)
 * |  (Slab lives here)   | (Dynamic Base, randomized by Limine)
 * |                      |
 * +----------------------+
 */

/*
 * 1. Kernel Image
 * Located in the top 2GB of address space.
 */
extern uint64_t g_kernel_virt_base;
#define KERNEL_VIRT_BASE g_kernel_virt_base
#define KERNEL_VIRT_END  0xFFFFFFFFFFFFFFFFULL
#define KERNEL_VIRT_SIZE (KERNEL_VIRT_END - KERNEL_VIRT_BASE + 1ULL)

/*
 * 2. HHDM (Higher Half Direct Map) & Slab
 * This is where Limine maps all physical memory.
 * The Slab allocator (kmalloc) returns addresses in this range.
 * Range: Dynamic (randomized by Limine)
 */
extern uint64_t g_hhdm_offset;
extern uint64_t g_hhdm_size;
#define HHDM_VIRT_BASE   g_hhdm_offset
#define HHDM_VIRT_LIMIT  (g_hhdm_offset + g_hhdm_size)
#define HHDM_VIRT_SIZE   g_hhdm_size

#define SLAB_VIRT_BASE   HHDM_VIRT_BASE
#define SLAB_VIRT_END    HHDM_VIRT_LIMIT
#define SLAB_VIRT_SIZE   HHDM_VIRT_SIZE

/*
 * 3. Vmalloc Allocator Region
 * Used by vmalloc() and viomap() for non-contiguous or IO mappings.
 * Starts at an offset from HHDM, ensuring it never overlaps.
 */
#ifndef CONFIG_VMALLOC_SIZE_GB
#define CONFIG_VMALLOC_SIZE_GB 64
#endif

extern uint64_t g_vmalloc_base;
extern uint64_t g_vmalloc_end;

#define VMALLOC_VIRT_BASE g_vmalloc_base
#define VMALLOC_VIRT_SIZE ((uint64_t)CONFIG_VMALLOC_SIZE_GB * 1024 * 1024 * 1024)
#define VMALLOC_VIRT_END  g_vmalloc_end

/* Helper to check if address is in kernel high memory */
static inline bool is_kernel_addr(uint64_t addr) {
  /* Canonical higher half check */
  return (int64_t)addr < 0;
}

static inline bool is_slab_addr(uint64_t addr) {
  return addr >= SLAB_VIRT_BASE && addr < SLAB_VIRT_END;
}

static inline bool is_vmalloc_addr(uint64_t addr) {
  return addr >= VMALLOC_VIRT_BASE && addr < VMALLOC_VIRT_END;
}

static inline bool is_pmm_addr(uint64_t addr) {
  /* HHDM is effectively the PMM addressable space */
  return addr >= HHDM_VIRT_BASE && addr < HHDM_VIRT_LIMIT;
}

static inline bool is_user_addr(uint64_t addr) {
  return !is_kernel_addr(addr);
}