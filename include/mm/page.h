#pragma once

#include <linux/list.h>
#include <kernel/types.h>

/* Page flags */
#define PG_reserved   (1 << 0)
#define PG_buddy      (1 << 1)
#define PG_active     (1 << 2)
#define PG_slab       (1 << 3)
#define PG_referenced (1 << 4)
#define PG_lru        (1 << 5)
#define PG_head       (1 << 6) /* Page is the head of a compound page (folio) */
#define PG_tail       (1 << 7) /* Page is a tail of a compound page */

struct kmem_cache;

#include <kernel/spinlock.h>

/**
 * struct page - Represents a physical page frame
 */
struct page {
  unsigned long flags; /* Page flags */
  union {
    struct list_head list; /* List node for free lists / generic */
    struct list_head lru;  /* Node in active/inactive lists */
    struct {
      struct page *next; /* Next page in a list (e.g. SLUB partial) */
      int pages; /* Number of pages (compound) */
      int pobjects; /* Approximate number of objects */
    };
  };

  union {
    struct {
      /* Page cache and anonymous pages */
      void *mapping;
      unsigned long index;
    };

    struct {
      /* Compound page head pointer (for tails) */
      struct page *head;
    };

    struct {
      /* SLUB */
      struct kmem_cache *slab_cache;
      void *freelist; /* First free object */
      union {
        unsigned counters;

        struct {
          unsigned inuse: 16;
          unsigned objects: 15;
          unsigned frozen: 1;
        };
      };
    };
  };

  unsigned int order; /* Order of the block (Buddy/Folio) */
  uint32_t zone; /* Memory zone (if any) */
  atomic_t _refcount; /* Reference count */

  /* Split page table lock */
  spinlock_t ptl;
};

/**
 * struct folio - Represents a contiguous set of pages managed as a unit.
 *
 * A folio is guaranteed to be a "head" page. It carries the mapping,
 * index, and reference count for the entire block.
 */
struct folio {
    struct page page;
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

#define PageHead(page)       ((page)->flags & PG_head)
#define SetPageHead(page)    ((page)->flags |= PG_head)
#define ClearPageHead(page)  ((page)->flags &= ~PG_head)

#define PageTail(page)       ((page)->flags & PG_tail)
#define SetPageTail(page)    ((page)->flags |= PG_tail)
#define ClearPageTail(page)  ((page)->flags &= ~PG_tail)

/* Reference counting */
#include <kernel/atomic.h>

static inline void get_page(struct page *page) {
  atomic_inc(&page->_refcount);
}

void put_page(struct page *page);

static inline int page_ref_count(struct page *page) {
  return atomic_read(&page->_refcount);
}

/* Folio-based reference counting */
static inline void folio_get(struct folio *folio) {
    atomic_inc(&folio->page._refcount);
}

static inline void folio_put(struct folio *folio) {
    put_page(&folio->page);
}

static inline int folio_ref_count(struct folio *folio) {
    return atomic_read(&folio->page._refcount);
}

static inline void folio_ref_add(struct folio *folio, int nr) {
    atomic_add(nr, &folio->page._refcount);
}

static inline struct folio *page_folio(struct page *page) {
    if (unlikely(PageTail(page)))
        return (struct folio *)page->head;
    return (struct folio *)page;
}

static inline struct page *folio_page(struct folio *folio, size_t n) {
    return &folio->page + n;
}

static inline unsigned int folio_order(struct folio *folio) {
    return folio->page.order;
}

static inline size_t folio_nr_pages(struct folio *folio) {
    return 1UL << folio->page.order;
}

static inline size_t folio_size(struct folio *folio) {
    return folio_nr_pages(folio) << 12; // PAGE_SHIFT
}

extern uint64_t g_hhdm_offset;
extern struct page *mem_map;

static inline void *page_address(struct page *page) {
    uint64_t pfn = (uint64_t)(page - mem_map);
    uint64_t phys = pfn << 12; // PAGE_SHIFT 12
    return (void *)(phys + g_hhdm_offset);
}

static inline void *folio_address(struct folio *folio) {
    return page_address(&folio->page);
}

static inline uint64_t folio_to_phys(struct folio *folio) {
    uint64_t pfn = (uint64_t)(&folio->page - mem_map);
    return pfn << 12;
}
