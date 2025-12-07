#pragma once

#include <kernel/types.h>

// Page size constants
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))

// Align macros
#define PAGE_ALIGN_DOWN(addr) ((addr) & PAGE_MASK)
#define PAGE_ALIGN_UP(addr) (((addr) + PAGE_SIZE - 1) & PAGE_MASK)

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
} pmm_stats_t;

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
 * @param stats Pointer to stats structure to fill
 */
void pmm_get_stats(pmm_stats_t *stats);

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

static inline void *pmm_phys_to_virt(uint64_t phys_addr) {
  return (void *)(phys_addr + g_hhdm_offset);
}

static inline uint64_t pmm_virt_to_phys(void *virt_addr) {
  return (uint64_t)virt_addr - g_hhdm_offset;
}
