#pragma once

#include <kernel/spinlock.h>
#include <kernel/atomic.h>
#include <linux/list.h>
#include <mm/page.h>
#include <arch/x64/cpu.h>
#include <mm/gfp.h>

#define MAX_ORDER 11

/* Zone types */
enum zone_type {
#ifdef CONFIG_ZONE_DMA
    ZONE_DMA,
#endif
#ifdef CONFIG_ZONE_DMA32
    ZONE_DMA32,
#endif
    ZONE_NORMAL,
    MAX_NR_ZONES
};

#ifndef CONFIG_ZONE_DMA
#define ZONE_DMA ZONE_NORMAL
#endif

#ifndef CONFIG_ZONE_DMA32
#define ZONE_DMA32 ZONE_NORMAL
#endif

/*
 * Page types for Per-CPU lists
 */
enum {
    PCP_LEFT_OVER,  /* Pages that didn't fit into other lists */
    PCP_HOT,        /* Hot pages (recently freed) */
    PCP_COLD,       /* Cold pages */
    PCP_TYPES
};

struct free_area {
    struct list_head free_list[1]; /* can extend to MIGRATE_TYPES later */
    unsigned long nr_free;
};

struct per_cpu_pages {
    int count;          /* number of pages in the list */
    int high;           /* high watermark, emptying needed */
    int batch;          /* chunk size for buddy add/remove */
    struct list_head list; /* the list of pages */
};

struct zone {
    /* Write-intensive fields used by page allocator */
    spinlock_t lock;
    
    struct free_area free_area[MAX_ORDER];
    
    /* Per-CPU page frame cache */
    struct per_cpu_pages pageset[MAX_CPUS];

    unsigned long zone_start_pfn;
    unsigned long spanned_pages;
    unsigned long present_pages;
    
    const char *name;
    
    /* Stats */
    atomic_long_t vm_stat[32]; // NR_FREE_PAGES, etc.
} __aligned(64); // Cache line aligned

/* Global array of zones */
extern struct zone managed_zones[MAX_NR_ZONES];

/*
 * Zone watermarks
 */
#define WMARK_MIN   0
#define WMARK_LOW   1
#define WMARK_HIGH  2

/*
 * Prototypes
 */
void free_area_init(void);
struct page *alloc_pages(gfp_t gfp_mask, unsigned int order);
void __free_pages(struct page *page, unsigned int order);
void free_pages(uint64_t addr, unsigned int order);

static inline struct page *alloc_page(gfp_t gfp_mask) {
    return alloc_pages(gfp_mask, 0);
}

static inline void __free_page(struct page *page) {
    __free_pages(page, 0);
}
