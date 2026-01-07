#pragma once

#include <kernel/types.h>
#include <arch/x86_64/mm/paging.h>
#include <mm/mm_types.h>

// Page Table Entry Flags
#define PTE_PRESENT (1ULL << 0)
#define PTE_RW (1ULL << 1)       // Read/Write (0 = Read-only)
#define PTE_USER (1ULL << 2)     // User/Supervisor (0 = Supervisor)
#define PTE_PWT (1ULL << 3)      // Page-Level Write-Through
#define PTE_PCD (1ULL << 4)      // Page-Level Cache Disable
#define PTE_ACCESSED (1ULL << 5) // Accessed
#define PTE_DIRTY (1ULL << 6)    // Dirty
#define PTE_HUGE (1ULL << 7)     // Huge Page (2MB/1GB)
#define PTE_PAT (1ULL << 7)      // Page Attribute Table (4KB pages)
#define PTE_GLOBAL (1ULL << 8)   // Global Translation
#define PTE_NX (1ULL << 63)      // No Execute

#define PDE_PAT (1ULL << 12)     // Page Attribute Table (2MB/1GB pages)

// CR3 / PCID bits
#define CR3_PCID_MASK 0xFFFULL
#define CR3_NOFLUSH   (1ULL << 63)

// Cache Types (PAT)
// 0: WB, 1: WC, 2: UC-, 3: UC, 4: WB, 5: WT, 6: WC, 7: WP
#define VMM_CACHE_WB (0)
#define VMM_CACHE_WC (PTE_PWT)
#define VMM_CACHE_UC_MINUS (PTE_PCD)
#define VMM_CACHE_UC (PTE_PCD | PTE_PWT)
#define VMM_CACHE_WT (PTE_PAT | PTE_PWT)
#define VMM_CACHE_WP (PTE_PAT | PTE_PCD | PTE_PWT)

// Address Masks
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000
#define PTE_GET_ADDR(pte) ((pte) & PTE_ADDR_MASK)
#define PTE_GET_FLAGS(pte) ((pte) & ~PTE_ADDR_MASK)

// Page Sizes
#define VMM_PAGE_SIZE_4K PAGE_SIZE
#define VMM_PAGE_SIZE_2M (2 * 1024 * 1024UL)
#define VMM_PAGE_SIZE_1G (1024 * 1024 * 1024UL)

// Virtual Memory Manager Interface

/**
 * Get current paging level (4 or 5)
 */
int vmm_get_paging_levels(void);

/**
 * Get the start of the canonical higher-half address space.
 * 4-level: 0xFFFF800000000000
 * 5-level: 0xFF00000000000000
 */
uint64_t vmm_get_canonical_high_base(void);

/**
 * Get the maximum valid user-space address.
 * 4-level: 0x00007FFFFFFFFFFF
 * 5-level: 0x00FFFFFFFFFFFFFF
 */
uint64_t vmm_get_max_user_address(void);

/**
 * Initialize the Virtual Memory Manager.
 * Creates a new PML4, maps the kernel and HHDM, and loads CR3.
 */
void vmm_init(void);

/**
 * Map a virtual page to a physical frame.
 *
 * @param mm        The address space to map in
 * @param virt      Virtual address to map (must be page aligned)
 * @param phys      Physical address to map to (must be page aligned)
 * @param flags     PTE flags (PTE_PRESENT | PTE_RW, etc.)
 * @return 0 on success, -1 on allocation failure
 */
int vmm_map_page(struct mm_struct *mm, uint64_t virt, uint64_t phys,
                 uint64_t flags);
int vmm_map_huge_page(struct mm_struct *mm, uint64_t virt, uint64_t phys,
                      uint64_t flags, uint64_t page_size);
int vmm_map_pages(struct mm_struct *mm, uint64_t virt, uint64_t phys,
                  size_t count, uint64_t flags);
int vmm_map_pages_list(struct mm_struct *mm, uint64_t virt, const uint64_t *phys_list,
                       size_t count, uint64_t flags);

/**
 * Unmap a virtual page.
 *
 * @param mm        The address space to unmap from
 * @param virt      Virtual address to unmap
 * @return 0 on success
 */
int vmm_unmap_page(struct mm_struct *mm, uint64_t virt);
uint64_t vmm_unmap_page_no_flush(struct mm_struct *mm, uint64_t virt);
int vmm_unmap_pages(struct mm_struct *mm, uint64_t virt, size_t count);
int vmm_unmap_pages_and_get_phys(struct mm_struct *mm, uint64_t virt,
                                 uint64_t *phys_list, size_t count);

/**
 * Copy user page tables for fork (COW).
 * @param src_mm Source address space
 * @param dst_mm Destination address space
 * @return 0 on success
 */
int vmm_copy_page_tables(struct mm_struct *src_mm, const struct mm_struct *dst_mm);

/**
 * Handle a Copy-On-Write fault.
 */
int vmm_handle_cow(struct mm_struct *mm, uint64_t virt);

/**
 * Get the physical address mapped to a virtual address. *
 * @param mm        Address space to query
 * @param virt      Virtual address query
 * @return Physical address, or 0 if not mapped
 */
uint64_t vmm_virt_to_phys(struct mm_struct *mm, uint64_t virt);

/**
 * Recursively free all page table levels for a given address space.
 * Only frees user-space mappings.
 * @param mm The address space to free.
 */
void vmm_free_page_tables(struct mm_struct *mm);

/**
 * Modern VMM functions
 */
int vmm_set_flags(struct mm_struct *mm, uint64_t virt, uint64_t flags);
int vmm_is_dirty(struct mm_struct *mm, uint64_t virt);
void vmm_clear_dirty(struct mm_struct *mm, uint64_t virt);
int vmm_is_accessed(struct mm_struct *mm, uint64_t virt);
void vmm_clear_accessed(struct mm_struct *mm, uint64_t virt);

/**
 * Huge Page Helpers
 */
struct mm_struct;
int vmm_merge_to_huge(struct mm_struct *mm, uint64_t virt, uint64_t target_huge_size);
int vmm_shatter_huge_page(struct mm_struct *mm, uint64_t virt, uint64_t large_page_size);
void vmm_merge_range(struct mm_struct *mm, uint64_t start, uint64_t end);

/**
 * Check if a specific page size is supported by the hardware.
 * @param size The page size in bytes (4KB, 2MB, 1GB).
 * @return 1 if supported, 0 otherwise.
 */
int vmm_page_size_supported(size_t size);

/**
 * Switch the active Page Table (CR3).
 *
 * @param pml_root_phys Physical address of the new PML root
 */
void vmm_switch_pml_root(uint64_t pml_root_phys);
void vmm_switch_pml_root_pcid(uint64_t pml_root_phys, uint16_t pcid, bool no_flush);

/* Smoke test */
void vmm_test(void);

extern uint64_t g_kernel_pml_root;