#pragma once

#include <kernel/types.h>

/**
 * Initialize the MMIO virtual address allocator.
 * Should be called during VMM initialization.
 */
void mmio_allocator_init(void);

/**
 * Map a physical MMIO region into virtual address space.
 * Automatically allocates virtual address space and creates page mappings.
 *
 * @param pml4_phys Physical address of the PML4 to map into
 * @param phys_addr Physical address of MMIO region
 * @param size Size of region in bytes
 * @return Virtual address of mapped region, or NULL on failure
 */
void *vmm_map_mmio(uint64_t pml4_phys, uint64_t phys_addr, size_t size);

/**
 * Unmap an MMIO region and free its virtual address space.
 * The virtual address space can be reused for future mappings.
 *
 * @param pml4_phys Physical address of the PML4
 * @param virt_addr Virtual address returned by vmm_map_mmio
 * @param size Size that was originally mapped
 */
void vmm_unmap_mmio(uint64_t pml4_phys, void *virt_addr, size_t size);

/**
 * Statistics for MMIO allocator
 */
typedef struct {
  uint64_t total_size;        // Total MMIO virtual space
  uint64_t allocated_size;    // Currently allocated
  uint64_t free_size;         // Currently free
  uint32_t num_allocations;   // Number of active allocations
  uint32_t num_free_regions;  // Number of free regions (fragmentation metric)
  uint32_t num_allocs;        // Total allocations performed
  uint32_t num_frees;         // Total frees performed
} mmio_stats_t;

/**
 * Get current MMIO allocator statistics
 */
void mmio_get_stats(mmio_stats_t *stats);

/**
 * Debug function to dump MMIO allocator state
 */
void mmio_dump_state(void);


extern uint64_t g_kernel_pml4; // Physical address of the kernel PML4

/**
 * Map MMIO region using the kernel's page table.
 * Simplified wrapper for drivers that always use kernel space.
 *
 * @param phys_addr Physical address of MMIO region
 * @param size Size in bytes
 * @return Virtual address, or NULL on failure
 */
static inline void *vmm_map_mmio_kernel(uint64_t phys_addr, size_t size) {
  return vmm_map_mmio(g_kernel_pml4, phys_addr, size);
}

/**
 * Unmap MMIO region from kernel space.
 *
 * @param virt_addr Virtual address returned by vmm_map_mmio_kernel
 * @param size Original size that was mapped
 */
static void vmm_unmap_mmio_kernel(void *virt_addr, size_t size) {
  vmm_unmap_mmio(g_kernel_pml4, virt_addr, size);
}
