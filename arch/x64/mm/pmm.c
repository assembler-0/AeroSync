/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/mm/pmm.c
 * @brief Physical Memory Manager (PMM) implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/**
 * Physical Memory Manager (PMM)
 *
 * Dynamic bitmap allocator that automatically detects memory size
 * and allocates bitmap from available memory. No hardcoded limits.
 * Parses Limine memory map and provides page allocation services.
 */

#include <compiler.h>
#include <arch/x64/cpu.h>
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/bitmap.h>
#include <limine/limine.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/paging.h>
#include <kernel/fkx/fkx.h>

// Global HHDM offset
uint64_t g_hhdm_offset = 0;
EXPORT_SYMBOL(g_hhdm_offset);

// Dynamic bitmap pointer - allocated during init
static unsigned long *pmm_bitmap = NULL;
static uint64_t pmm_bitmap_size_words = 0;
static uint64_t pmm_max_pages = 0;

// PMM state
static volatile pmm_stats_t pmm_stats __aligned(16);
static spinlock_t pmm_lock = 0;
static bool pmm_initialized = false;

// First free page hint for faster allocation
static uint64_t pmm_first_free_page = 0;

// Helper: Mark page as used
static inline void pmm_mark_used(uint64_t page) {
  if (page < pmm_max_pages) {
    set_bit(page, pmm_bitmap);
  }
}

// Helper: Mark page as free
static inline void pmm_mark_free(uint64_t page) {
  if (page < pmm_max_pages) {
    clear_bit(page, pmm_bitmap);
  }
}

// Helper: Test if page is used
static inline bool pmm_is_used(uint64_t page) {
  if (page >= pmm_max_pages)
    return true; // Out of range = used
  return test_bit(page, pmm_bitmap);
}

// Convert Limine memory type to string
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

// Find suitable memory region for bitmap
static struct limine_memmap_entry *find_bitmap_location(
    struct limine_memmap_response *memmap, uint64_t required_bytes) {

  struct limine_memmap_entry *best_region = NULL;
  uint64_t best_size = 0;

  // Find the largest usable region that can fit the bitmap
  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];

    if (entry->type != LIMINE_MEMMAP_USABLE) {
      continue;
    }

    uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
    uint64_t aligned_end = PAGE_ALIGN_DOWN(entry->base + entry->length);

    if (aligned_end <= aligned_base) {
      continue;
    }

    uint64_t available = aligned_end - aligned_base;

    // Need at least required_bytes
    if (available >= required_bytes && available > best_size) {
      best_size = available;
      best_region = entry;
    }
  }

  return best_region;
}

int pmm_init(void *memmap_response_ptr, uint64_t hhdm_offset) {
  struct limine_memmap_response *memmap =
      (struct limine_memmap_response *)memmap_response_ptr;

  if (!memmap || memmap->entry_count == 0) {
    printk(PMM_CLASS "Error: Invalid memory map\n");
    return -1;
  }

  g_hhdm_offset = hhdm_offset;

  printk(KERN_DEBUG PMM_CLASS "Initializing with HHDM offset: 0x%llx\n", hhdm_offset);
  printk(KERN_DEBUG PMM_CLASS "Memory map has %llu entries\n", memmap->entry_count);

  // First pass: Find highest physical address to determine bitmap size
  uint64_t highest_addr = 0;
  uint64_t total_usable_bytes = 0;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];
    uint64_t end = entry->base + entry->length;

    if (end > highest_addr) {
      highest_addr = end;
    }

    if (entry->type == LIMINE_MEMMAP_USABLE) {
      uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
      uint64_t aligned_end = PAGE_ALIGN_DOWN(end);
      if (aligned_end > aligned_base) {
        total_usable_bytes += aligned_end - aligned_base;
      }
    }
  }

  printk(KERN_DEBUG PMM_CLASS "Detected highest address: 0x%llx\n", highest_addr);
  printk(PMM_CLASS "Total usable memory: %llu MB\n",
         total_usable_bytes / (1024 * 1024));

  // Calculate bitmap size based on highest address
  pmm_max_pages = PHYS_TO_PFN(highest_addr);
  if (highest_addr & (PAGE_SIZE - 1)) {
    pmm_max_pages++; // Round up if not page-aligned
  }

  pmm_bitmap_size_words = (pmm_max_pages + BITS_PER_LONG - 1) / BITS_PER_LONG;
  uint64_t bitmap_bytes = pmm_bitmap_size_words * sizeof(unsigned long);
  uint64_t bitmap_bytes_aligned = PAGE_ALIGN_UP(bitmap_bytes);

  printk(KERN_DEBUG PMM_CLASS "Max pages: %llu\n", pmm_max_pages);
  printk(KERN_DEBUG PMM_CLASS "Bitmap size: %llu bytes (%llu pages)\n",
         bitmap_bytes, bitmap_bytes_aligned / PAGE_SIZE);

  // Find memory region for bitmap
  struct limine_memmap_entry *bitmap_region =
      find_bitmap_location(memmap, bitmap_bytes_aligned);

  if (!bitmap_region) {
    printk(PMM_CLASS "Error: Cannot find suitable memory for bitmap\n");
    return -1;
  }

  // Allocate bitmap at beginning of chosen region
  uint64_t bitmap_phys = PAGE_ALIGN_UP(bitmap_region->base);
  pmm_bitmap = (unsigned long *)pmm_phys_to_virt(bitmap_phys);

  printk(KERN_DEBUG PMM_CLASS "Bitmap allocated at phys: 0x%llx, virt: %p\n",
         bitmap_phys, pmm_bitmap);

  // Initialize bitmap - mark all pages as used initially
  memset(pmm_bitmap, 0xFF, bitmap_bytes);

  // Initialize stats
  memset((void *)&pmm_stats, 0, sizeof(pmm_stats));
  pmm_stats.bitmap_pages = bitmap_bytes_aligned / PAGE_SIZE;
  pmm_stats.bitmap_size = bitmap_bytes;

  // Second pass: Mark usable pages as free
  uint64_t total_free_pages = 0;
  uint64_t bitmap_start_page = PHYS_TO_PFN(bitmap_phys);
  uint64_t bitmap_end_page = bitmap_start_page + pmm_stats.bitmap_pages;

  for (uint64_t i = 0; i < memmap->entry_count; i++) {
    struct limine_memmap_entry *entry = memmap->entries[i];

    if (entry->type != LIMINE_MEMMAP_USABLE) {
      continue;
    }

    uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
    uint64_t aligned_end = PAGE_ALIGN_DOWN(entry->base + entry->length);

    if (aligned_end <= aligned_base) {
      continue;
    }

    uint64_t start_page = PHYS_TO_PFN(aligned_base);
    uint64_t end_page = PHYS_TO_PFN(aligned_end);

    if (end_page > pmm_max_pages) {
      end_page = pmm_max_pages;
    }

    // Mark pages as free, but skip bitmap region
    for (uint64_t page = start_page; page < end_page; page++) {
      // Don't free bitmap pages
      if (page >= bitmap_start_page && page < bitmap_end_page) {
        continue;
      }

      pmm_mark_free(page);
      total_free_pages++;

      // Update first free hint
      if (pmm_first_free_page == 0 || page < pmm_first_free_page) {
        pmm_first_free_page = page;
      }
    }
  }

  pmm_stats.total_pages = total_free_pages + pmm_stats.bitmap_pages;
  pmm_stats.free_pages = total_free_pages;
  pmm_stats.used_pages = pmm_stats.bitmap_pages; // Bitmap is already used
  pmm_stats.total_bytes = total_usable_bytes;
  pmm_stats.highest_address = highest_addr;

  pmm_initialized = true;

  printk(PMM_CLASS "PMM initialized successfully\n");
  printk(PMM_CLASS "Total pages: %llu (%llu MB)\n",
         pmm_stats.total_pages,
         (pmm_stats.total_pages * PAGE_SIZE) / (1024 * 1024));
  printk(PMM_CLASS "Free pages: %llu (%llu MB)\n",
         pmm_stats.free_pages,
         (pmm_stats.free_pages * PAGE_SIZE) / (1024 * 1024));
  printk(KERN_DEBUG PMM_CLASS "Bitmap overhead: %llu pages (%llu KB)\n",
         pmm_stats.bitmap_pages,
         (pmm_stats.bitmap_pages * PAGE_SIZE) / 1024);

  return 0;
}

uint64_t pmm_alloc_page(void) {
  if (!pmm_initialized) {
    return 0;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  // Start search from hint
  uint64_t start = pmm_first_free_page;

  // Use generic bitmap to find first free page
  int page = find_next_zero_bit(pmm_bitmap, pmm_max_pages, start);
  if (page >= pmm_max_pages) {
    // Wrap around and search from beginning
    page = find_first_zero_bit(pmm_bitmap, start);
  }

  if (page < pmm_max_pages) {
    pmm_mark_used(page);
    pmm_stats.free_pages--;
    pmm_stats.used_pages++;

    // Update hint to next potential free page
    pmm_first_free_page = page + 1;
    if (pmm_first_free_page >= pmm_max_pages) {
      pmm_first_free_page = 0;
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);

    // Zero the page before returning
    void *virt = pmm_phys_to_virt(PFN_TO_PHYS(page));
    memset(virt, 0, PAGE_SIZE);

    return PFN_TO_PHYS(page);
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);

  printk(KERN_CRIT PMM_CLASS "Warning: Out of physical memory!\n");
  printk(KERN_CRIT PMM_CLASS "Free: %llu, Used: %llu, Total: %llu\n",
         pmm_stats.free_pages, pmm_stats.used_pages, pmm_stats.total_pages);
  return 0;
}

uint64_t pmm_alloc_pages(size_t count) {
  if (!pmm_initialized || count == 0) {
    return 0;
  }

  if (count == 1) {
    return pmm_alloc_page();
  }

  // Quick check if we have enough free pages at all
  if (pmm_stats.free_pages < count) {
    printk(KERN_CRIT PMM_CLASS
           "Warning: Not enough memory (%llu free, %zu requested)\n",
           pmm_stats.free_pages, count);
    return 0;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  // Search for contiguous free pages
  for (uint64_t page = pmm_first_free_page;
       page + count <= pmm_max_pages; page++) {

    bool found = true;

    // Check if 'count' contiguous pages are free
    for (size_t i = 0; i < count; i++) {
      if (pmm_is_used(page + i)) {
        found = false;
        page += i; // Skip ahead past this used page
        break;
      }
    }

    if (found) {
      // Mark all pages as used
      for (size_t i = 0; i < count; i++) {
        pmm_mark_used(page + i);
      }

      pmm_stats.free_pages -= count;
      pmm_stats.used_pages += count;

      // Update hint
      if (page == pmm_first_free_page) {
        pmm_first_free_page = page + count;
        if (pmm_first_free_page >= pmm_max_pages) {
          pmm_first_free_page = 0;
        }
      }

      spinlock_unlock_irqrestore(&pmm_lock, flags);

      // Zero all pages
      void *virt = pmm_phys_to_virt(PFN_TO_PHYS(page));
      memset(virt, 0, count * PAGE_SIZE);

      return PFN_TO_PHYS(page);
    }
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);

  printk(KERN_CRIT PMM_CLASS
         "Warning: Cannot allocate %zu contiguous pages\n", count);
  printk(KERN_CRIT PMM_CLASS "Free: %llu (fragmented)\n",
         pmm_stats.free_pages);
  return 0;
}

void pmm_free_page(uint64_t phys_addr) {
  pmm_free_pages(phys_addr, 1);
}

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

  if (start_page >= pmm_max_pages) {
    printk(KERN_ERR PMM_CLASS
           "Error: Free address 0x%llx out of range (max page: %llu)\n",
           phys_addr, pmm_max_pages);
    return;
  }

  if (start_page + count > pmm_max_pages) {
    printk(KERN_ERR PMM_CLASS
           "Error: Free range exceeds max pages (page %llu + %zu > %llu)\n",
           start_page, count, pmm_max_pages);
    return;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

  for (size_t i = 0; i < count; i++) {
    uint64_t page = start_page + i;

    if (!pmm_is_used(page)) {
      printk(KERN_ERR PMM_CLASS
             "Warning: Double-free detected at page 0x%llx (phys: 0x%llx)\n",
             page, PFN_TO_PHYS(page));
      continue;
    }

    pmm_mark_free(page);
    pmm_stats.free_pages++;
    pmm_stats.used_pages--;

    // Update hint if this page is lower
    if (page < pmm_first_free_page) {
      pmm_first_free_page = page;
    }
  }

  spinlock_unlock_irqrestore(&pmm_lock, flags);
}

pmm_stats_t * pmm_get_stats(void) {
  return (pmm_stats_t*)(&pmm_stats);
}
#include <kernel/fkx/fkx.h>
EXPORT_SYMBOL(pmm_virt_to_phys);
EXPORT_SYMBOL(pmm_phys_to_virt);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
