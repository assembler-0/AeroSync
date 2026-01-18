#pragma once

#include <aerosync/atomic.h>
#include <aerosync/types.h>
#include <linux/list.h>

/* Page flags */
#define PG_reserved (1 << 0)
#define PG_buddy (1 << 1)
#define PG_active (1 << 2)
#define PG_slab (1 << 3)
#define PG_referenced (1 << 4)
#define PG_lru (1 << 5)
#define PG_head (1 << 6)   /* Page is the head of a compound page (folio) */
#define PG_tail (1 << 7)   /* Page is a tail of a compound page */
#define PG_dirty (1 << 8)  /* Page has been modified and needs writeback */
#define PG_locked (1 << 9) /* Page is locked (bit-spinlock for SLUB) */

struct kmem_cache;

#include <aerosync/spinlock.h>

/**
 * struct page - Represents a physical page frame
 */
struct page {
  unsigned long flags; /* Page flags */

  union {
    struct list_head list; /* List node for free lists / generic */
    struct list_head lru;  /* Node in active/inactive lists */
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
          unsigned inuse : 16;
          unsigned objects : 15;
          unsigned frozen : 1;
        };
      };
    };
  };

  uint16_t order;       /* Order of the block (Buddy/Folio) */
  uint16_t migratetype; /* Migration type for Buddy */
  uint32_t zone;        /* Memory zone (if any) */
  uint32_t node;        /* NUMA node ID */
  atomic_t _refcount;   /* Reference count */

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
  union {
    struct {
      unsigned long flags;

      union {
        struct list_head lru;
      };

      void *mapping;
      unsigned long index;
      void *private;
      unsigned int order;
      uint32_t zone;
      uint32_t node;
      atomic_t _refcount;
    };

    struct page page;
  };
};

/* Helper macros */
#define PageReserved(page) ((page)->flags & PG_reserved)
#define SetPageReserved(page) ((page)->flags |= PG_reserved)
#define ClearPageReserved(page) ((page)->flags &= ~PG_reserved)

#define PageBuddy(page) ((page)->flags & PG_buddy)
#define SetPageBuddy(page) ((page)->flags |= PG_buddy)
#define ClearPageBuddy(page) ((page)->flags &= ~PG_buddy)

#define PageSlab(page) ((page)->flags & PG_slab)
#define SetPageSlab(page) ((page)->flags |= PG_slab)
#define ClearPageSlab(page) ((page)->flags &= ~PG_slab)

#define PageHead(page) ((page)->flags & PG_head)
#define SetPageHead(page) ((page)->flags |= PG_head)
#define ClearPageHead(page) ((page)->flags &= ~PG_head)

#define PageTail(page) ((page)->flags & PG_tail)
#define SetPageTail(page) ((page)->flags |= PG_tail)
#define ClearPageTail(page) ((page)->flags &= ~PG_tail)

#define PageLocked(page)     ((page)->flags & PG_locked)

/*
 * Bit-spinlock operations for per-page locking (SLUB allocator)
 * These provide lockless synchronization at page granularity.
 */
static inline void lock_page_slab(struct page *page) {
  while (__atomic_test_and_set((volatile void *)&page->flags + (PG_locked / 8), 
                                __ATOMIC_ACQUIRE)) {
    /* Spin with pause instruction to reduce contention */
    __builtin_ia32_pause();
  }
}

static inline void unlock_page_slab(struct page *page) {
  __atomic_clear((volatile void *)&page->flags + (PG_locked / 8), 
                 __ATOMIC_RELEASE);
}

static inline int trylock_page_slab(struct page *page) {
  return !__atomic_test_and_set((volatile void *)&page->flags + (PG_locked / 8), 
                                 __ATOMIC_ACQUIRE);
}

/* Reference counting */

void put_page(struct page *page);

static inline int page_ref_count(struct page *page) {
  return atomic_read(&page->_refcount);
}

/* Folio-based reference counting */
static inline void folio_get(struct folio *folio) {
  atomic_inc(&folio->page._refcount);
}

static inline void folio_put(struct folio *folio) { put_page(&folio->page); }

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

static inline void get_page(struct page *page) {
  if (unlikely(!page)) return;
  struct folio *folio = page_folio(page);
  atomic_inc(&folio->page._refcount);
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

static inline unsigned long folio_pfn(struct folio *folio) {
  return (unsigned long)(&folio->page - mem_map);
}

static inline uint64_t folio_to_phys(struct folio *folio) {
  uint64_t pfn = (uint64_t)(&folio->page - mem_map);
  return pfn << 12;
}

static inline uint64_t page_to_phys(struct page *page) {
  uint64_t pfn = (uint64_t)(page - mem_map);
  return pfn << 12;
}