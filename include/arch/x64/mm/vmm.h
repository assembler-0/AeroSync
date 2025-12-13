#pragma once

#include <kernel/types.h>

/*
 * x86_64 Paging Definitions
 */

#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))

// Page Table Entry Flags
#define PTE_PRESENT (1ULL << 0)
#define PTE_RW (1ULL << 1)       // Read/Write (0 = Read-only)
#define PTE_USER (1ULL << 2)     // User/Supervisor (0 = Supervisor)
#define PTE_PWT (1ULL << 3)      // Page-Level Write-Through
#define PTE_PCD (1ULL << 4)      // Page-Level Cache Disable
#define PTE_ACCESSED (1ULL << 5) // Accessed
#define PTE_DIRTY (1ULL << 6)    // Dirty
#define PTE_HUGE (1ULL << 7)     // Huge Page (2MB/1GB)
#define PTE_GLOBAL (1ULL << 8)   // Global Translation
#define PTE_NX (1ULL << 63)      // No Execute

// Table Indices
#define PML4_INDEX(virt) (((virt) >> 39) & 0x1FF)
#define PDPT_INDEX(virt) (((virt) >> 30) & 0x1FF)
#define PD_INDEX(virt) (((virt) >> 21) & 0x1FF)
#define PT_INDEX(virt) (((virt) >> 12) & 0x1FF)

// Address Masks
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000
#define PTE_GET_ADDR(pte) ((pte) & PTE_ADDR_MASK)
#define PTE_GET_FLAGS(pte) ((pte) & ~PTE_ADDR_MASK)

// Virtual Memory Manager Interface

/**
 * Initialize the Virtual Memory Manager.
 * Creates a new PML4, maps the kernel and HHDM, and loads CR3.
 */
void vmm_init(void);

/**
 * Map a virtual page to a physical frame.
 *
 * @param pml4_phys Physical address of the PML4 table
 * @param virt      Virtual address to map (must be page aligned)
 * @param phys      Physical address to map to (must be page aligned)
 * @param flags     PTE flags (PTE_PRESENT | PTE_RW, etc.)
 * @return 0 on success, -1 on allocation failure
 */
int vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                 uint64_t flags);

/**
 * Unmap a virtual page.
 *
 * @param pml4_phys Physical address of the PML4 table
 * @param virt      Virtual address to unmap
 * @return 0 on success
 */
int vmm_unmap_page(uint64_t pml4_phys, uint64_t virt);

/**
 * Get the physical address mapped to a virtual address.
 *
 * @param pml4_phys Physical address of the PML4 table
 * @param virt      Virtual address query
 * @return Physical address, or 0 if not mapped
 */
uint64_t vmm_virt_to_phys(uint64_t pml4_phys, uint64_t virt);

/**
 * Switch the active Page Table (CR3).
 *
 * @param pml4_phys Physical address of the new PML4
 */
void vmm_switch_pml4(uint64_t pml4_phys);

extern uint64_t g_kernel_pml4;