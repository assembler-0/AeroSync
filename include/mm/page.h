#pragma once

#include <linux/list.h>
#include <kernel/types.h>

/* Page flags */
#define PG_reserved   (1 << 0)
#define PG_buddy      (1 << 1)
#define PG_active     (1 << 2)
#define PG_slab       (1 << 3)

struct kmem_cache;

/**
 * struct page - Represents a physical page frame
 */
struct page {
    struct list_head list;    /* List node for free lists */
    unsigned int order;       /* Order of the block if it's the head of a buddy block */
    uint32_t flags;           /* Page flags */
    uint32_t zone;            /* Memory zone (if any) */
    atomic_t _refcount;       /* Reference count */
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
