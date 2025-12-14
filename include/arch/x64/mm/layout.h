#pragma once

#include <kernel/types.h>

/*
 * VoidFrameX Memory Map (x86_64)
 *
 * Virtual Address Space Layout:
 *
 * +----------------------+ 0xFFFFFFFFFFFFFFFF
 * |                      |
 * |    Kernel Text/Data  | (2GB, defined by linker script)
 * |  0xFFFFFFFF80000000  |
 * +----------------------+
 * |                      |
 * |        Unused        |
 * |                      |
 * +----------------------+ 0xFFFF901000000000
 * |                      |
 * |       Vmalloc        | (64GB)
 * |  0xFFFF900000000000  |
 * +----------------------+ 0xFFFF900000000000
 * |                      |
 * |         Slab         | (1GB)
 * |  0xFFFF800000000000  |
 * +----------------------+ 0xFFFF800000000000
 * |                      |
 * |        HHDM          | (HHDM_OFFSET, usually 0xFFFF800000000000 approx)
 * |  (Dynamic Base)      |
 * +----------------------+
 */

/* Kernel Image Base (Typical for x86_64 higher half) */
#define KERNEL_VIRT_BASE 0xFFFFFFFF80000000UL
#define KERNEL_VIRT_SIZE (2UL * 1024 * 1024 * 1024)

/* Slab Allocator Region */
#define SLAB_VIRT_BASE 0xFFFF800000000000UL
#define SLAB_VIRT_SIZE (1UL << 30) // 1GB
#define SLAB_VIRT_END (SLAB_VIRT_BASE + SLAB_VIRT_SIZE)

/* Vmalloc Allocator Region */
/* Note: kept separate from Slab to ensure easier debugging/isolation */
#define VMALLOC_VIRT_BASE 0xFFFF900000000000UL
#define VMALLOC_VIRT_SIZE (64UL * 1024 * 1024 * 1024) // 64GB
#define VMALLOC_VIRT_END (VMALLOC_VIRT_BASE + VMALLOC_VIRT_SIZE)

/* Helper to check if address is in kernel high memory */
static inline bool is_kernel_addr(uint64_t addr) {
  return addr >= 0xFFFF000000000000UL;
}
