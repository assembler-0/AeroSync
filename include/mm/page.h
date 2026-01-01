#pragma once

#include <linux/list.h>
#include <kernel/types.h>

/* Page flags */
#define PG_reserved   (1 << 0)
#define PG_buddy      (1 << 1)
#define PG_active     (1 << 2)
#define PG_slab       (1 << 3)

struct kmem_cache;

#include <kernel/spinlock.h>

/**
 * struct page - Represents a physical page frame
 */
struct page {
    unsigned long flags;      /* Page flags */
    union {
        struct list_head list;    /* List node for free lists */
        struct {
            struct page *next;    /* Next page in a list (e.g. SLUB partial) */
            int pages;            /* Number of pages (compound) */
            int pobjects;         /* Approximate number of objects */
        };
    };

    union {
        struct { /* Page cache and anonymous pages */
            void *mapping;
            unsigned long index;
        };
        struct { /* SLUB */
            struct kmem_cache *slab_cache;
            void *freelist;       /* First free object */
            union {
                unsigned counters;
                struct {
                    unsigned inuse:16;
                    unsigned objects:15;
                    unsigned frozen:1;
                };
            };
        };
    };

    unsigned int order;       /* Order of the block if it's the head of a buddy block */
    uint32_t zone;            /* Memory zone (if any) */
    atomic_t _refcount;       /* Reference count */

    /* Split page table lock */
    spinlock_t ptl;
};

/* Helper macros */
#define PageReserved(page)   ((page)->flags & PG_reserved)
#define SetPageReserved(page) ((page)->flags |= PG_reserved)
#define ClearPageReserved(page) ((page)->flags &= ~PG_reserved)

#define PageBuddy(page)      ((page)->flags & PG_buddy)
#define SetPageBuddy(page)    ((page)->flags |= PG_buddy)
#define ClearPageBuddy(page)  ((page)->flags &= ~PG_buddy)

#define PageSlab(page)       ((page)->flags & PG_slab)
#define SetPageSlab(page)    ((page)->flags |= PG_slab)
#define ClearPageSlab(page)  ((page)->flags &= ~PG_slab)

/* Reference counting */
#include <kernel/atomic.h>

static inline void get_page(struct page *page) {
    atomic_inc(&page->_refcount);
}

void put_page(struct page *page);

static inline int page_ref_count(struct page *page) {
    return atomic_read(&page->_refcount);
}
