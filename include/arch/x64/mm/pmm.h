#pragma once

#include <kernel/types.h>
#include <mm/page.h>
#include <arch/x64/mm/paging.h>

#define MAX_ORDER 19

// Convert between addresses and page frame numbers
#define PHYS_TO_PFN(addr) ((addr) >> PAGE_SHIFT)
#define PFN_TO_PHYS(pfn) ((pfn) << PAGE_SHIFT)

// Memory region types (matching Limine)
typedef enum {
  MEM_USABLE = 0,
  MEM_RESERVED = 1,
  MEM_ACPI_RECLAIMABLE = 2,
  MEM_ACPI_NVS = 3,
  MEM_BAD_MEMORY = 4,
  MEM_BOOTLOADER_RECLAIMABLE = 5,
  MEM_KERNEL_AND_MODULES = 6,
  MEM_FRAMEBUFFER = 7,
  MEM_ACPI_TABLES = 8,
} mem_region_type_t;

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
int pmm_init(void *memmap_response, uint64_t hhdm_offset);

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

static inline void *pmm_phys_to_virt(uint64_t phys_addr) {
  return (void *)(phys_addr + g_hhdm_offset);
}

static inline uint64_t pmm_virt_to_phys(void *virt_addr) {
  return (uint64_t)virt_addr - g_hhdm_offset;
}

static inline struct page *phys_to_page(uint64_t phys) {
  return &mem_map[PHYS_TO_PFN(phys)];
}

static inline uint64_t page_to_pfn(struct page *page) {
  return (uint64_t)(page - mem_map);
}

static inline struct page *virt_to_page(void *addr) {
  return phys_to_page(pmm_virt_to_phys(addr));
}

/* Simple PMM smoke test */
void pmm_test(void);