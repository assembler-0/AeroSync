#pragma once

#include <aerosync/types.h>
#include <mm/page.h>
#include <arch/x86_64/mm/paging.h>

#define MAX_ORDER 19

// Convert between addresses and page frame numbers
#define PHYS_TO_PFN(addr) ((addr) >> PAGE_SHIFT)
#define PFN_TO_PHYS(pfn) ((pfn) << PAGE_SHIFT)

// PMM statistics
typedef struct {
  uint64_t total_pages;     // Total physical pages
  uint64_t free_pages;      // Currently free pages
  uint64_t used_pages;      // Currently used pages
  uint64_t reserved_pages;  // Reserved (unusable) pages
  uint64_t total_bytes;     // Total usable memory in bytes
  uint64_t highest_address; // Highest usable physical address
  uint64_t memmap_pages;    // Pages used by mem_map array
  uint64_t memmap_size;     // Size of mem_map array in bytes
} pmm_stats_t;

#include <mm/zone.h>

/* Per-CPU page cache for order-0 pages */
/* Defined in mm/zone.h */

/**
 * Initialize the physical memory manager.
 * Must be called early in kernel initialization.
 *
 * @param memmap_response Pointer to Limine memory map response
 * @param hhdm_offset The HHDM offset from Limine
 * @return 0 on success, negative on error
 */
int pmm_init(void *memmap_response, uint64_t hhdm_offset, void *rsdp);

/**
 * Initialize per-CPU PMM state (PCP list).
 * Must be called on each CPU after per-CPU area setup.
 */
void pmm_init_cpu(void);

/**
 * Allocate a single physical page.
 *
 * @return Physical address of the allocated page, or 0 on failure
 */
uint64_t pmm_alloc_page(void);

/**
 * Allocate multiple contiguous physical pages.
 *
 * @param count Number of pages to allocate
 * @return Physical address of the first page, or 0 on failure
 */
uint64_t pmm_alloc_pages(size_t count);

/**
 * Allocate a physical huge page of a specific size.
 * Automatically checks for architectural support and uses fail-fast paths.
 * 
 * @param size Size in bytes (e.g., VMM_PAGE_SIZE_2M, VMM_PAGE_SIZE_1G)
 * @return Physical address of the block, or 0 on failure.
 */
uint64_t pmm_alloc_huge(size_t size);

/**
 * Free a single physical page.
 *
 * @param phys_addr Physical address of the page to free
 */
void pmm_free_page(uint64_t phys_addr);

/**
 * Free multiple contiguous physical pages.
 *
 * @param phys_addr Physical address of the first page
 * @param count Number of pages to free
 */
void pmm_free_pages(uint64_t phys_addr, size_t count);

/**
 * Get current PMM statistics.
 *
 * @return Pointer to stats structure
 */
pmm_stats_t *pmm_get_stats(void);

/**
 * Convert physical address to virtual address using HHDM.
 *
 * @param phys_addr Physical address
 * @return Virtual address in HHDM region
 */
static inline void *pmm_phys_to_virt(uint64_t phys_addr);

/**
 * Convert virtual address (in HHDM) to physical address.
 *
 * @param virt_addr Virtual address in HHDM region
 * @return Physical address
 */
static inline uint64_t pmm_virt_to_phys(void *virt_addr);

// HHDM offset - set during pmm_init

extern uint64_t g_hhdm_offset;
extern struct page *mem_map;
extern uint64_t pmm_max_pages;

static inline void *pmm_phys_to_virt(uint64_t phys_addr) {
  return (void *)(phys_addr + g_hhdm_offset);
}

static inline uint64_t pmm_virt_to_phys(void *virt_addr) {
  return (uint64_t)virt_addr - g_hhdm_offset;
}

static inline struct page *phys_to_page(uint64_t phys) {
  uint64_t pfn = PHYS_TO_PFN(phys);
  if (pfn >= pmm_max_pages) return NULL;
  return &mem_map[pfn];
}

static inline uint64_t page_to_pfn(struct page *page) {
  return (uint64_t)(page - mem_map);
}

static inline struct page *virt_to_page(void *addr) {
  return phys_to_page(pmm_virt_to_phys(addr));
}

/* Simple PMM smoke test */

void pmm_test(void);



/**

 * Report system memory capabilities and potential limitations.

 */

void pmm_report_capabilities(void);
