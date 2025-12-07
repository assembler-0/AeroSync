/**
 * Physical Memory Manager (PMM)
 *
 * Uses a bitmap allocator to track free/used physical pages.
 * Parses Limine memory map and provides page allocation services.
 */

#include "arch/x64/cpu.h"
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <limine/limine.h>
#include <mm/pmm.h>

// Maximum physical memory we support (16 GB - increase if needed)
// 16GB with 4KB pages = 4M pages = 512KB bitmap
#define PMM_MAX_MEMORY (16ULL * 1024 * 1024 * 1024)
#define PMM_MAX_PAGES (PMM_MAX_MEMORY / PAGE_SIZE)

// Bitmap: 1 bit per page, 8 pages per byte
#define BITMAP_SIZE_BYTES (PMM_MAX_PAGES / 8)

// Global HHDM offset
uint64_t g_hhdm_offset = 0;

// Bitmap stored in BSS - 8MB for 256GB support
// Bit set = page used, Bit clear = page free
static uint8_t pmm_bitmap[BITMAP_SIZE_BYTES];

// PMM state
static volatile pmm_stats_t pmm_stats __attribute__((aligned(16)));
static spinlock_t pmm_lock = 0;
static bool pmm_initialized = false;

// First free page hint for faster allocation
static uint64_t pmm_first_free_page = 0;

// Helper: Set a bit in the bitmap (mark page as used)
static inline void bitmap_set(uint64_t page) {
  if (page < PMM_MAX_PAGES) {
    pmm_bitmap[page / 8] |= (1 << (page % 8));
  }
}

// Helper: Clear a bit in the bitmap (mark page as free)
static inline void bitmap_clear(uint64_t page) {
  if (page < PMM_MAX_PAGES) {
    pmm_bitmap[page / 8] &= ~(1 << (page % 8));
  }
}

// Helper: Test if a bit is set (page is used)
static inline bool bitmap_test(uint64_t page) {
  if (page >= PMM_MAX_PAGES)
    return true; // Out of range = used
  return (pmm_bitmap[page / 8] & (1 << (page % 8))) != 0;
}

// Convert Limine memory type to our type
static const char *memtype_to_string(uint64_t type) {
  switch (type) {
  case LIMINE_MEMMAP_USABLE:
    return "Usable";
  case LIMINE_MEMMAP_RESERVED:
    return "Reserved";
  case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
    return "ACPI Reclaimable";
  case LIMINE_MEMMAP_ACPI_NVS:
    return "ACPI NVS";
  case LIMINE_MEMMAP_BAD_MEMORY:
    return "Bad Memory";
  case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
    return "Bootloader Reclaimable";
  case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
    return "Kernel/Modules";
  case LIMINE_MEMMAP_FRAMEBUFFER:
    return "Framebuffer";
  case LIMINE_MEMMAP_ACPI_TABLES:
    return "ACPI Tables";
  default:
    return "Unknown";
  }
}

int pmm_init(void *memmap_response_ptr, uint64_t hhdm_offset) {
  struct limine_memmap_response *memmap =
      (struct limine_memmap_response *)memmap_response_ptr;

  if (!memmap || memmap->entry_count == 0) {
    printk(PMM_CLASS "Error: Invalid memory map\n");
    return -1;
  }

  g_hhdm_offset = hhdm_offset;

  printk(PMM_CLASS "Initializing with HHDM offset: 0x%llx\n", hhdm_offset);
  printk(PMM_CLASS "Memory map has %llu entries\n", memmap->entry_count);
  printk(PMM_CLASS "Bitmap at %p, size %u bytes\n", pmm_bitmap,
         (unsigned int)BITMAP_SIZE_BYTES);

  // First pass: Mark ALL pages as used
  printk(PMM_CLASS "Initializing bitmap...\n");
  memset(pmm_bitmap, 0xFF, BITMAP_SIZE_BYTES);
  printk(PMM_CLASS "Bitmap initialized.\n");

  // Initialize stats
  memset((void *)&pmm_stats, 0, sizeof(pmm_stats));
  printk(PMM_CLASS "Scanning memory map...\n");

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);
  // Second pass: Parse memory map and mark usable pages as free
  uint64_t total_usable = 0;
  uint64_t highest_addr = 0;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    uint64_t base = entry->base;
    uint64_t length = entry->length;
    uint64_t type = entry->type;
    uint64_t end = base + length;

    // Track highest address
    if (end > highest_addr && type == LIMINE_MEMMAP_USABLE) {
      highest_addr = end;
    }

    // Only mark USABLE memory as free
    if (type == LIMINE_MEMMAP_USABLE) {
      // Align to page boundaries
      uint64_t aligned_base = PAGE_ALIGN_UP(base);
      uint64_t aligned_end = PAGE_ALIGN_DOWN(end);

      if (aligned_end > aligned_base) {
        uint64_t start_page = PHYS_TO_PFN(aligned_base);
        uint64_t end_page = PHYS_TO_PFN(aligned_end);

        // Cap at max supported if needed
        if (start_page >= PMM_MAX_PAGES) {
          continue;
        }
        if (end_page > PMM_MAX_PAGES) {
          end_page = PMM_MAX_PAGES;
        }

        uint64_t num_pages = end_page - start_page;

        // Simple loop to avoid optimization bugs
        for (uint64_t p = start_page; p < end_page; p++) {
          bitmap_clear(p);
        }

        pmm_stats.free_pages += num_pages;
        total_usable += num_pages * PAGE_SIZE;

        // Update first free hint
        if (pmm_first_free_page == 0 || start_page < pmm_first_free_page) {
          pmm_first_free_page = start_page;
        }
      }
    }
  }

  pmm_stats.total_pages = pmm_stats.free_pages;
  pmm_stats.total_bytes = total_usable;
  pmm_stats.highest_address = highest_addr;
  pmm_stats.used_pages = 0;
  pmm_stats.reserved_pages = 0;

  pmm_initialized = true;

  spinlock_unlock_irqrestore(&pmm_lock, flags);

  printk(PMM_CLASS "PMM initialized.\n");
  printk(PMM_CLASS "Initialized: %llu MB usable (%llu pages)\n",
         total_usable / (1024 * 1024), pmm_stats.free_pages);
  printk(PMM_CLASS "Highest usable address: 0x%llx\n", highest_addr);

  return 0;
}

uint64_t pmm_alloc_page(void) {
  if (!pmm_initialized) {
    return 0;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  // Start search from hint
  uint64_t start = pmm_first_free_page;

  // Search for a free page
  for (uint64_t page = start; page < PMM_MAX_PAGES; page++) {
    if (!bitmap_test(page)) {
      // Found free page
      bitmap_set(page);
      pmm_stats.free_pages--;
      pmm_stats.used_pages++;

      // Update hint
      if (page == pmm_first_free_page) {
        pmm_first_free_page = page + 1;
      }

      spinlock_unlock_irqrestore(&pmm_lock, flags);

      // Zero the page before returning (security measure)
      void *virt = pmm_phys_to_virt(PFN_TO_PHYS(page));
      memset(virt, 0, PAGE_SIZE);

      return PFN_TO_PHYS(page);
    }
  }

  // Wrap around and search from beginning
  for (uint64_t page = 0; page < start; page++) {
    if (!bitmap_test(page)) {
      bitmap_set(page);
      pmm_stats.free_pages--;
      pmm_stats.used_pages++;
      pmm_first_free_page = page + 1;

      spinlock_unlock_irqrestore(&pmm_lock, flags);

      void *virt = pmm_phys_to_virt(PFN_TO_PHYS(page));
      memset(virt, 0, PAGE_SIZE);

      return PFN_TO_PHYS(page);
    }
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);

  printk(KERN_CRIT PMM_CLASS "Warning: Out of physical memory!\n");
  return 0;
}

uint64_t pmm_alloc_pages(size_t count) {
  if (!pmm_initialized || count == 0) {
    return 0;
  }

  if (count == 1) {
    return pmm_alloc_page();
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  // Search for contiguous free pages
  for (uint64_t page = pmm_first_free_page; page + count <= PMM_MAX_PAGES;
       page++) {
    bool found = true;

    // Check if 'count' contiguous pages are free
    for (size_t i = 0; i < count; i++) {
      if (bitmap_test(page + i)) {
        found = false;
        page += i; // Skip to after this used page
        break;
      }
    }

    if (found) {
      // Mark all pages as used
      for (size_t i = 0; i < count; i++) {
        bitmap_set(page + i);
      }

      pmm_stats.free_pages -= count;
      pmm_stats.used_pages += count;

      // Update hint
      if (page == pmm_first_free_page) {
        pmm_first_free_page = page + count;
      }

      spinlock_unlock_irqrestore(&pmm_lock, flags);

      // Zero all pages
      void *virt = pmm_phys_to_virt(PFN_TO_PHYS(page));
      memset(virt, 0, count * PAGE_SIZE);

      return PFN_TO_PHYS(page);
    }
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);

  printk(KERN_CRIT PMM_CLASS "Warning: Cannot allocate %zu contiguous pages\n",
         count);
  return 0;
}

void pmm_free_page(uint64_t phys_addr) { pmm_free_pages(phys_addr, 1); }

void pmm_free_pages(uint64_t phys_addr, size_t count) {
  if (!pmm_initialized || count == 0) {
    return;
  }

  // Validate address is page-aligned
  if (phys_addr & (PAGE_SIZE - 1)) {
    printk(KERN_ERR PMM_CLASS
           "Error: Attempted to free unaligned address 0x%llx\n",
           phys_addr);
    return;
  }

  uint64_t start_page = PHYS_TO_PFN(phys_addr);

  if (start_page + count > PMM_MAX_PAGES) {
    printk(KERN_ERR PMM_CLASS "Error: Free address out of range 0x%llx\n",
           phys_addr);
    return;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  for (size_t i = 0; i < count; i++) {
    uint64_t page = start_page + i;

    if (!bitmap_test(page)) {
      printk(KERN_ERR PMM_CLASS "Warning: Double-free of page 0x%llx\n",
             PFN_TO_PHYS(page));
      continue;
    }

    bitmap_clear(page);
    pmm_stats.free_pages++;
    pmm_stats.used_pages--;

    // Update hint if this page is lower
    if (page < pmm_first_free_page) {
      pmm_first_free_page = page;
    }
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);
}

pmm_stats_t * pmm_get_stats() {
  return (pmm_stats_t*)(&pmm_stats);
}
