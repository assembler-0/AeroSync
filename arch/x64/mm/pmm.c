/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/mm/pmm.c
 * @brief High performance PMM
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

#include <compiler.h>
#include <arch/x64/cpu.h>
#include <drivers/uart/serial.h>
#include <kernel/classes.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <limine/limine.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/paging.h>
#include <kernel/fkx/fkx.h>
#include <mm/page.h>
#include <linux/container_of.h>

// Global HHDM offset
uint64_t g_hhdm_offset = 0;
EXPORT_SYMBOL(g_hhdm_offset);

// Buddy system structures
struct free_area {
    struct list_head free_list;
    unsigned long nr_free;
};

static struct free_area free_area[MAX_ORDER];
struct page *mem_map = NULL;
static uint64_t pmm_max_pages = 0;

// PMM state
static volatile pmm_stats_t pmm_stats __aligned(16);
static spinlock_t pmm_lock = 0;
static bool pmm_initialized = false;

// Helper: Get order for a given number of pages
static inline unsigned int get_order(size_t count) {
    if (count <= 1) return 0;
    return 64 - __builtin_clzll(count - 1);
}

// Helper: Get page from PFN
static inline struct page *pfn_to_page(uint64_t pfn) {
    if (unlikely(pfn >= pmm_max_pages)) return NULL;
    return &mem_map[pfn];
}

// Check if a page can be a buddy for another at a given order
static inline bool page_is_buddy(struct page *page, unsigned int order) {
    if (PageBuddy(page) && page->order == order)
        return true;
    return false;
}

/**
 * __free_pages_core - Core buddy system free/merge function
 * @pfn: Page frame number to free
 * @order: Order of the block being freed
 */
static void __free_pages_core(uint64_t pfn, unsigned int order) {
    uint64_t buddy_pfn;
    uint64_t combined_pfn;
    struct page *page, *buddy;

    page = &mem_map[pfn];
    ClearPageReserved(page);

    while (order < MAX_ORDER - 1) {
        buddy_pfn = pfn ^ (1 << order);
        if (buddy_pfn >= pmm_max_pages)
            break;

        buddy = &mem_map[buddy_pfn];
        if (!page_is_buddy(buddy, order))
            break;

        /* Found a free buddy of the same order, merge them */
        list_del(&buddy->list);
        free_area[order].nr_free--;
        ClearPageBuddy(buddy);

        combined_pfn = buddy_pfn & pfn;
        pfn = combined_pfn;
        page = &mem_map[pfn];
        order++;
    }

    SetPageBuddy(page);
    page->order = order;
    list_add(&page->list, &free_area[order].free_list);
    free_area[order].nr_free++;
}

/**
 * pmm_alloc_pages_buddy - Allocate a power-of-two block of pages
 * @order: Desired order (2^order pages)
 */
static struct page *pmm_alloc_pages_buddy(unsigned int order) {
    unsigned int current_order;
    struct page *page;

    for (current_order = order; current_order < MAX_ORDER; current_order++) {
        if (list_empty(&free_area[current_order].free_list))
            continue;

        /* Found a block in this order list */
        page = list_first_entry(&free_area[current_order].free_list, struct page, list);
        list_del(&page->list);
        free_area[current_order].nr_free--;
        ClearPageBuddy(page);

        /* Split the block until we reach the desired order */
        while (current_order > order) {
            current_order--;
            struct page *buddy = &page[1 << current_order];
            buddy->order = current_order;
            SetPageBuddy(buddy);
            list_add(&buddy->list, &free_area[current_order].free_list);
            free_area[current_order].nr_free++;
        }

        page->order = order;
        return page;
    }

    return NULL;
}

// Find suitable memory region for mem_map array
static struct limine_memmap_entry *find_memmap_location(
    struct limine_memmap_response *memmap, uint64_t required_bytes) {

    struct limine_memmap_entry *best_region = NULL;
    uint64_t best_size = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
        uint64_t aligned_end = PAGE_ALIGN_DOWN(entry->base + entry->length);
        if (aligned_end <= aligned_base) continue;

        uint64_t available = aligned_end - aligned_base;
        if (available >= required_bytes && available > best_size) {
            best_size = available;
            best_region = entry;
        }
    }
    return best_region;
}

int pmm_init(void *memmap_response_ptr, uint64_t hhdm_offset) {
    struct limine_memmap_response *memmap = (struct limine_memmap_response *)memmap_response_ptr;

    if (!memmap || memmap->entry_count == 0) {
        printk(PMM_CLASS "Error: Invalid memory map\n");
        return -1;
    }

    g_hhdm_offset = hhdm_offset;
    printk(KERN_DEBUG PMM_CLASS "Initializing Buddy System PMM with HHDM offset: 0x%llx\n", hhdm_offset);

    // Pass 1: Determine total memory and highest address
    uint64_t highest_addr = 0;
    uint64_t total_usable_bytes = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        uint64_t end = entry->base + entry->length;

        // Only calculate highest address from regions we might actually use
        // to avoid mem_map explosion if there is a huge gap before high MMIO
        if (entry->type == LIMINE_MEMMAP_USABLE ||
            entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            if (end > highest_addr) highest_addr = end;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            uint64_t aligned_base = PAGE_ALIGN_UP(entry->base);
            uint64_t aligned_end = PAGE_ALIGN_DOWN(end);
            if (aligned_end > aligned_base) total_usable_bytes += aligned_end - aligned_base;
        }
    }

    pmm_max_pages = PHYS_TO_PFN(PAGE_ALIGN_UP(highest_addr));
    uint64_t memmap_size = pmm_max_pages * sizeof(struct page);
    uint64_t memmap_pages = PAGE_ALIGN_UP(memmap_size) / PAGE_SIZE;

    printk(KERN_DEBUG PMM_CLASS "Max PFN: %llu, Memmap size: %llu KB (%llu pages)\n",
           pmm_max_pages, memmap_size / 1024, memmap_pages);

    // Find location for mem_map array
    struct limine_memmap_entry *mm_region = find_memmap_location(memmap, PAGE_ALIGN_UP(memmap_size));
    if (!mm_region) {
        printk(PMM_CLASS "Error: Cannot find suitable memory for mem_map\n");
        return -1;
    }

    uint64_t mm_phys = PAGE_ALIGN_UP(mm_region->base);
    mem_map = (struct page *)pmm_phys_to_virt(mm_phys);
    memset(mem_map, 0, memmap_size);

    // Initialize all pages as reserved initially
    for (uint64_t i = 0; i < pmm_max_pages; i++) {
        INIT_LIST_HEAD(&mem_map[i].list);
        mem_map[i].flags = PG_reserved;
        mem_map[i].order = 0;
    }

    // Initialize free area lists
    for (int i = 0; i < MAX_ORDER; i++) {
        INIT_LIST_HEAD(&free_area[i].free_list);
        free_area[i].nr_free = 0;
    }

    // Pass 2: Populate buddy system with usable memory
    uint64_t mm_start_pfn = PHYS_TO_PFN(mm_phys);
    uint64_t mm_end_pfn = mm_start_pfn + memmap_pages;
    uint64_t usable_pages = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if (entry->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t start_pfn = PHYS_TO_PFN(PAGE_ALIGN_UP(entry->base));
        uint64_t end_pfn = PHYS_TO_PFN(PAGE_ALIGN_DOWN(entry->base + entry->length));

        for (uint64_t pfn = start_pfn; pfn < end_pfn; pfn++) {
            // Reserve page 0 (Null address protection and legacy BIOS compatibility)
            if (pfn == 0) continue;

            // Skip mem_map itself
            if (pfn >= mm_start_pfn && pfn < mm_end_pfn) continue;
            
            __free_pages_core(pfn, 0);
            usable_pages++;
        }
    }

    // Initialize statistics
    pmm_stats.total_pages = usable_pages + memmap_pages;
    pmm_stats.free_pages = usable_pages;
    pmm_stats.used_pages = memmap_pages;
    pmm_stats.total_bytes = total_usable_bytes;
    pmm_stats.highest_address = highest_addr;
    pmm_stats.memmap_pages = memmap_pages;
    pmm_stats.memmap_size = memmap_size;

    pmm_initialized = true;

    printk(PMM_CLASS "Buddy System PMM initialized successfully\n");
    printk(PMM_CLASS "Total memory: %llu MB, Free: %llu MB\n",
           (pmm_stats.total_pages * PAGE_SIZE) / (1024 * 1024),
           (pmm_stats.free_pages * PAGE_SIZE) / (1024 * 1024));

    return 0;
}

uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

uint64_t pmm_alloc_pages(size_t count) {
    if (unlikely(!pmm_initialized || count == 0)) return 0;

    unsigned int order = get_order(count);
    if (unlikely(order >= MAX_ORDER)) {
        printk(KERN_ERR PMM_CLASS "Requested allocation too large: %zu pages\n", count);
        return 0;
    }

    irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);
    struct page *page = pmm_alloc_pages_buddy(order);
    
    if (unlikely(!page)) {
        spinlock_unlock_irqrestore(&pmm_lock, flags);
        printk(KERN_CRIT PMM_CLASS "Out of physical memory (order %u)\n", order);
        return 0;
    }

    uint64_t pfn = page_to_pfn(page);
    uint64_t allocated_pages = 1UL << order;
    
    pmm_stats.free_pages -= allocated_pages;
    pmm_stats.used_pages += allocated_pages;

    /* Sophisticated: Return extra pages to buddy system if we allocated a larger block than needed */
    if (allocated_pages > count) {
        uint64_t extra_start_pfn = pfn + count;
        uint64_t extra_pages_count = allocated_pages - count;

        for (uint64_t i = 0; i < extra_pages_count; i++) {
            __free_pages_core(extra_start_pfn + i, 0);
            pmm_stats.free_pages++;
            pmm_stats.used_pages--;
        }
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);

    uint64_t phys = PFN_TO_PHYS(pfn);
    void *virt = pmm_phys_to_virt(phys);
    memset(virt, 0, count * PAGE_SIZE);

    return phys;
}

void pmm_free_page(uint64_t phys_addr) {
    pmm_free_pages(phys_addr, 1);
}

void pmm_free_pages(uint64_t phys_addr, size_t count) {
    if (unlikely(!pmm_initialized || count == 0)) return;

    if (unlikely(phys_addr & (PAGE_SIZE - 1))) {
        printk(KERN_ERR PMM_CLASS "Freeing unaligned address 0x%llx\n", phys_addr);
        return;
    }

    uint64_t pfn = PHYS_TO_PFN(phys_addr);
    irq_flags_t flags = spinlock_lock_irqsave(&pmm_lock);

    for (size_t i = 0; i < count; i++) {
        uint64_t curr_pfn = pfn + i;
        if (unlikely(curr_pfn >= pmm_max_pages)) break;

        struct page *page = &mem_map[curr_pfn];
        if (unlikely(PageBuddy(page))) {
            printk(KERN_ERR PMM_CLASS "Double-free detected at PFN 0x%llx\n", curr_pfn);
            continue;
        }

        __free_pages_core(curr_pfn, 0);
        pmm_stats.free_pages++;
        pmm_stats.used_pages--;
    }

    spinlock_unlock_irqrestore(&pmm_lock, flags);
}

pmm_stats_t *pmm_get_stats(void) {
    return (pmm_stats_t *)(&pmm_stats);
}

EXPORT_SYMBOL(pmm_virt_to_phys);
EXPORT_SYMBOL(pmm_phys_to_virt);
EXPORT_SYMBOL(pmm_alloc_page);
EXPORT_SYMBOL(pmm_free_page);
EXPORT_SYMBOL(pmm_alloc_pages);
EXPORT_SYMBOL(pmm_free_pages);