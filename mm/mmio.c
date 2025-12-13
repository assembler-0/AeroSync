/**
 * MMIO Virtual Address Allocator
 *
 * Manages virtual address space for MMIO mappings using a free list
 * approach. Supports allocation, freeing, and coalescing of adjacent
 * free regions to prevent fragmentation.
 */

#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>

// MMIO virtual address range
#define MMIO_VIRT_BASE   0xFFFF900000000000ULL
#define MMIO_VIRT_END    0xFFFF9FFFFFFFFFFFULL
#define MMIO_TOTAL_SIZE  (MMIO_VIRT_END - MMIO_VIRT_BASE)

// Free region node (stored in the free region itself)
typedef struct mmio_region {
  uint64_t virt_addr;           // Starting virtual address
  size_t size;                  // Size in bytes
  struct mmio_region *next;     // Next free region
  struct mmio_region *prev;     // Previous free region
} mmio_region_t;

// Allocated region tracking (stored separately)
typedef struct mmio_alloc {
  uint64_t virt_addr;           // Starting virtual address
  size_t size;                  // Size in bytes (for unmapping)
  struct mmio_alloc *next;      // Next allocated region
} mmio_alloc_t;

// MMIO allocator state
static mmio_region_t *mmio_free_list = NULL;
static mmio_alloc_t *mmio_alloc_list = NULL;
static spinlock_t mmio_lock = 0;
static bool mmio_allocator_initialized = false;

// Statistics
typedef struct {
  uint64_t total_size;
  uint64_t allocated_size;
  uint64_t free_size;
  uint32_t num_allocations;
  uint32_t num_free_regions;
  uint32_t num_allocs;
  uint32_t num_frees;
} mmio_stats_t;

static mmio_stats_t mmio_stats = {0};

// Initialize the MMIO allocator
void mmio_allocator_init(void) {
  if (mmio_allocator_initialized) {
    return;
  }

  // Create initial free region spanning entire MMIO space
  // We need physical memory to store the region node
  uint64_t node_phys = pmm_alloc_page();
  if (!node_phys) {
    printk(MMIO_CLASS "Failed to allocate initial region node\n");
    return;
  }

  mmio_free_list = (mmio_region_t *)pmm_phys_to_virt(node_phys);
  mmio_free_list->virt_addr = MMIO_VIRT_BASE;
  mmio_free_list->size = MMIO_TOTAL_SIZE;
  mmio_free_list->next = NULL;
  mmio_free_list->prev = NULL;

  mmio_stats.total_size = MMIO_TOTAL_SIZE;
  mmio_stats.free_size = MMIO_TOTAL_SIZE;
  mmio_stats.num_free_regions = 1;

  mmio_allocator_initialized = true;

  printk(MMIO_CLASS "MMIO allocator initialized (range: 0x%llx - 0x%llx, size: %llu GB)\n",
         MMIO_VIRT_BASE, MMIO_VIRT_END, MMIO_TOTAL_SIZE / (1024ULL * 1024 * 1024));
}

// Allocate a node for tracking (from a separate pool)
static mmio_alloc_t *mmio_alloc_node(void) {
  uint64_t node_phys = pmm_alloc_page();
  if (!node_phys) {
    return NULL;
  }

  mmio_alloc_t *node = (mmio_alloc_t *)pmm_phys_to_virt(node_phys);
  memset(node, 0, sizeof(mmio_alloc_t));
  return node;
}

static void mmio_free_node(mmio_alloc_t *node) {
  uint64_t phys = pmm_virt_to_phys(node);
  pmm_free_page(phys);
}

// Find and allocate virtual address space
static uint64_t mmio_alloc_virt_locked(size_t size) {
  if (!mmio_allocator_initialized) {
    return 0;
  }

  // Align size to page boundary
  size = PAGE_ALIGN_UP(size);

  // Search free list for suitable region (first-fit)
  mmio_region_t *region = mmio_free_list;
  while (region) {
    if (region->size >= size) {
      uint64_t alloc_addr = region->virt_addr;

      // If exact fit, remove region from free list
      if (region->size == size) {
        if (region->prev) {
          region->prev->next = region->next;
        } else {
          mmio_free_list = region->next;
        }

        if (region->next) {
          region->next->prev = region->prev;
        }

        // Free the region node
        uint64_t node_phys = pmm_virt_to_phys(region);
        pmm_free_page(node_phys);

        mmio_stats.num_free_regions--;
      } else {
        // Shrink region
        region->virt_addr += size;
        region->size -= size;
      }

      // Track allocation
      mmio_alloc_t *alloc = mmio_alloc_node();
      if (!alloc) {
        // Failed to allocate tracking node - this is bad
        // In production, you'd want to handle this better
        printk(MMIO_CLASS "Warning - failed to allocate tracking node\n");
      } else {
        alloc->virt_addr = alloc_addr;
        alloc->size = size;
        alloc->next = mmio_alloc_list;
        mmio_alloc_list = alloc;
        mmio_stats.num_allocations++;
      }

      mmio_stats.allocated_size += size;
      mmio_stats.free_size -= size;
      mmio_stats.num_allocs++;

      return alloc_addr;
    }
    region = region->next;
  }

  return 0; // No suitable region found
}

// Free virtual address space and coalesce adjacent regions
static void mmio_free_virt_locked(uint64_t virt_addr, size_t size) {
  if (!mmio_allocator_initialized) {
    return;
  }

  // Align size to page boundary
  size = PAGE_ALIGN_UP(size);

  // Remove from allocation list
  mmio_alloc_t *alloc = mmio_alloc_list;
  mmio_alloc_t *prev_alloc = NULL;

  while (alloc) {
    if (alloc->virt_addr == virt_addr) {
      if (prev_alloc) {
        prev_alloc->next = alloc->next;
      } else {
        mmio_alloc_list = alloc->next;
      }

      mmio_free_node(alloc);
      mmio_stats.num_allocations--;
      break;
    }
    prev_alloc = alloc;
    alloc = alloc->next;
  }

  // Create new free region
  uint64_t new_node_phys = pmm_alloc_page();
  if (!new_node_phys) {
    printk(MMIO_CLASS "Warning - failed to allocate free region node\n");
    return;
  }

  mmio_region_t *new_region = (mmio_region_t *)pmm_phys_to_virt(new_node_phys);
  new_region->virt_addr = virt_addr;
  new_region->size = size;
  new_region->next = NULL;
  new_region->prev = NULL;

  // Insert into free list (sorted by address) and coalesce
  if (!mmio_free_list) {
    mmio_free_list = new_region;
    mmio_stats.num_free_regions = 1;
  } else {
    mmio_region_t *current = mmio_free_list;
    mmio_region_t *prev = NULL;

    // Find insertion point
    while (current && current->virt_addr < virt_addr) {
      prev = current;
      current = current->next;
    }

    // Insert new region
    new_region->next = current;
    new_region->prev = prev;

    if (prev) {
      prev->next = new_region;
    } else {
      mmio_free_list = new_region;
    }

    if (current) {
      current->prev = new_region;
    }

    mmio_stats.num_free_regions++;

    // Coalesce with next region if adjacent
    if (current && (new_region->virt_addr + new_region->size == current->virt_addr)) {
      new_region->size += current->size;
      new_region->next = current->next;

      if (current->next) {
        current->next->prev = new_region;
      }

      uint64_t node_phys = pmm_virt_to_phys(current);
      pmm_free_page(node_phys);
      mmio_stats.num_free_regions--;
    }

    // Coalesce with previous region if adjacent
    if (prev && (prev->virt_addr + prev->size == new_region->virt_addr)) {
      prev->size += new_region->size;
      prev->next = new_region->next;

      if (new_region->next) {
        new_region->next->prev = prev;
      }

      uint64_t node_phys = pmm_virt_to_phys(new_region);
      pmm_free_page(node_phys);
      mmio_stats.num_free_regions--;
    }
  }

  mmio_stats.allocated_size -= size;
  mmio_stats.free_size += size;
  mmio_stats.num_frees++;
}

// Public API: Map MMIO region
void *vmm_map_mmio(uint64_t pml4_phys, uint64_t phys_addr, size_t size) {
  if (!mmio_allocator_initialized) {
    mmio_allocator_init();
  }

  irq_flags_t flags = spinlock_lock_irqsave(&mmio_lock);

  // Align start/end to page boundaries
  uint64_t phys_start = PAGE_ALIGN_DOWN(phys_addr);
  uint64_t phys_end = PAGE_ALIGN_UP(phys_addr + size);
  uint64_t aligned_size = phys_end - phys_start;
  uint64_t offset_in_page = phys_addr - phys_start;

  // Allocate virtual address space
  uint64_t virt_start = mmio_alloc_virt_locked(aligned_size);
  if (!virt_start) {
    spinlock_unlock_irqrestore(&mmio_lock, flags);
    printk(MMIO_CLASS "Failed to allocate virtual space for %zu bytes\n", aligned_size);
    return NULL;
  }

  spinlock_unlock_irqrestore(&mmio_lock, flags);

  // Map each page (using external VMM function)
  // Note: You'll need to expose vmm_map_page_locked or use vmm_map_page
  for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
    uint64_t pte_flags = PTE_PRESENT | PTE_RW | PTE_PCD;
    if (vmm_map_page(pml4_phys, virt_start + i, phys_start + i, pte_flags) != 0) {
      // Mapping failed - cleanup
      printk(MMIO_CLASS "Failed to map page at virt 0x%llx\n", virt_start + i);

      // Unmap what we've mapped so far
      for (uint64_t j = 0; j < i; j += PAGE_SIZE) {
        vmm_unmap_page(pml4_phys, virt_start + j);
      }

      // Free virtual space
      flags = spinlock_lock_irqsave(&mmio_lock);
      mmio_free_virt_locked(virt_start, aligned_size);
      spinlock_unlock_irqrestore(&mmio_lock, flags);

      return NULL;
    }
  }

  // Return virtual address with original offset applied
  return (void *)(virt_start + offset_in_page);
}

// Public API: Unmap MMIO region
void vmm_unmap_mmio(uint64_t pml4_phys, void *virt_addr, size_t size) {
  if (!mmio_allocator_initialized) {
    return;
  }

  uint64_t virt_start = PAGE_ALIGN_DOWN((uint64_t)virt_addr);
  uint64_t virt_end = PAGE_ALIGN_UP((uint64_t)virt_addr + size);
  uint64_t aligned_size = virt_end - virt_start;

  // Unmap all pages
  for (uint64_t v = virt_start; v < virt_end; v += PAGE_SIZE) {
    vmm_unmap_page(pml4_phys, v);
  }

  // Free virtual address space
  irq_flags_t flags = spinlock_lock_irqsave(&mmio_lock);
  mmio_free_virt_locked(virt_start, aligned_size);
  spinlock_unlock_irqrestore(&mmio_lock, flags);
}

// Get MMIO allocator statistics
void mmio_get_stats(mmio_stats_t *stats) {
  if (!stats) {
    return;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&mmio_lock);
  memcpy(stats, &mmio_stats, sizeof(mmio_stats_t));
  spinlock_unlock_irqrestore(&mmio_lock, flags);
}

// Debug: Print MMIO allocator state
void mmio_dump_state(void) {
  irq_flags_t flags = spinlock_lock_irqsave(&mmio_lock);

  printk(MMIO_CLASS "MMIO Allocator State:\n");
  printk(MMIO_CLASS "  Total size: %llu MB\n", mmio_stats.total_size / (1024 * 1024));
  printk(MMIO_CLASS "  Allocated: %llu MB (%u allocations)\n",
         mmio_stats.allocated_size / (1024 * 1024),
         mmio_stats.num_allocations);
  printk(MMIO_CLASS "  Free: %llu MB (%u regions)\n",
         mmio_stats.free_size / (1024 * 1024),
         mmio_stats.num_free_regions);
  printk(MMIO_CLASS "  Operations: %u allocs, %u frees\n",
         mmio_stats.num_allocs, mmio_stats.num_frees);

  printk(MMIO_CLASS "\nFree regions:\n");
  mmio_region_t *region = mmio_free_list;
  int count = 0;
  while (region && count < 20) {
    printk(MMIO_CLASS "  [%d] 0x%llx - 0x%llx (%zu KB)\n",
           count,
           region->virt_addr,
           region->virt_addr + region->size,
           region->size / 1024);
    region = region->next;
    count++;
  }
  if (region) {
    printk(MMIO_CLASS "  ... (%u more regions)\n", mmio_stats.num_free_regions - count);
  }

  spinlock_unlock_irqrestore(&mmio_lock, flags);
}