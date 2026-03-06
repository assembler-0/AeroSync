#pragma once

#include <aerosync/atomic.h>
#include <aerosync/compiler.h>
#include <aerosync/types.h>
#include <linux/list.h>

/* Page flags */
#define PG_reserved (1 << 0)
#define PG_buddy (1 << 1)
#define PG_active (1 << 2)
#define PG_slab (1 << 3)
#define PG_referenced (1 << 4)
#define PG_lru (1 << 5)
#define PG_head (1 << 6)      /* Page is the head of a compound page (folio) */
#define PG_tail (1 << 7)      /* Page is a tail of a compound page */
#define PG_dirty (1 << 8)     /* Page has been modified and needs writeback */
#define PG_locked (1 << 9)    /* Page is locked (bit-spinlock for SLUB) */
#define PG_poisoned (1 << 10) /* Page has been poisoned by kernel */
#define PG_zeroed (1 << 11)   /* Page has been pre-zeroed by background thread */

/* MGLRU flags (bits 50-53) */
#define LRU_GEN_MASK 0x7ULL
#define LRU_GEN_SHIFT 50
#define LRU_REFS_MASK 0x3ULL
#define LRU_REFS_SHIFT 53
#define LRU_REFS_FLAGS (LRU_REFS_MASK << LRU_REFS_SHIFT)

struct kmem_cache;

#include <aerosync/spinlock.h>

/**
 * struct page - Represents a physical page frame
 *
 * This structure is exactly 64 bytes (one cache line) to minimize overhead
 * and optimize memory access.
 */
struct page {
  unsigned long flags; /* Page flags (0-8) */

  union {
    struct list_head list; /* List node for free lists / generic (8-24) */
    struct list_head lru;  /* Node in active/inactive lists */
  };

  /* Metadata union: Mutually exclusive usage (24-40) */
  union {
    struct {
      /* Page cache and anonymous pages */
      void *mapping;
      unsigned long index;
    };

    struct {
      /* Compound page head pointer (for tails) */
      struct page *head;
      unsigned long _unused_1;
    };

    struct {
      /* SLUB */
      struct kmem_cache *slab_cache;
      void *freelist; /* First free object */
    };

    struct {
      /* Split page table lock */
      spinlock_t ptl;
      unsigned long _unused_2;
    };
  };

  /* Secondary metadata union (40-48) */
  union {
    void *private; /* Generic private data (e.g. for swap/filesystems) */

    /* SLUB counters - unionized with private to save space */
    struct {
      unsigned short inuse;
      unsigned short objects;
      unsigned char frozen;
      unsigned char _pad;
    };
    unsigned int counters;
  };

  struct resdomain *rd; /* Resource domain that owns this page (48-56) */

  atomic_t _refcount; /* Reference count (56-60) */

  /* Topology metadata (60-64) - Packed into remaining 32 bits */
  struct {
    unsigned order : 6;       /* Order of the block (0-63) */
    unsigned migratetype : 4; /* Migration type for Buddy */
    unsigned zone : 6;        /* Memory zone index */
    unsigned node : 16;       /* NUMA node ID */
  } __packed;
} __packed __aligned(CACHE_LINE_SIZE);

static_assert(sizeof(struct page) == 64, "struct page must be exactly 64 bytes");
static_assert(alignof(struct page) == 64,
               "struct page must be cache-line aligned");

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
      struct resdomain *rd;
      atomic_t _refcount;
      struct {
        unsigned order : 6;
        unsigned migratetype : 4;
        unsigned zone : 6;
        unsigned node : 16;
      } __packed;
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

#define PageLocked(page) ((page)->flags & PG_locked)

#define PagePoisoned(page) ((page)->flags & PG_poisoned)
#define SetPagePoisoned(page) ((page)->flags |= PG_poisoned)
#define ClearPagePoisoned(page) ((page)->flags &= ~PG_poisoned)

#define offset_in_page(p) (((unsigned long)p) % PAGE_SIZE)

/*
 * Bit-spinlock operations for per-page locking (SLUB allocator)
 * These provide lockless synchronization at page granularity.
 */
static inline void lock_page_slab(struct page *page) {
  /* Use bit-level atomic OR to lock without overwriting other flags */
  while (__atomic_fetch_or(&page->flags, PG_locked, __ATOMIC_ACQUIRE) &
         PG_locked) {
    cpu_relax();
  }
}

static inline void unlock_page_slab(struct page *page) {
  /* Clear the bit atomically */
  __atomic_fetch_and(&page->flags, ~((unsigned long)PG_locked),
                     __ATOMIC_RELEASE);
}

static inline int trylock_page_slab(struct page *page) {
  /* Return 1 on success (previous bit was 0) */
  return !(__atomic_fetch_or(&page->flags, PG_locked, __ATOMIC_ACQUIRE) &
           PG_locked);
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

static inline bool folio_try_get(struct folio *folio) {
  return atomic_inc_not_zero(&folio->page._refcount);
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
  if (unlikely(!page))
    return;
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
extern uint64_t empty_zero_page; /* Physical address of the global zero page */

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

/* LRU management */
void folio_add_lru(struct folio *folio);
void lru_batch_flush_cpu(void);

/* syscall */
int sys_move_pages(pid_t pid, unsigned long nr_pages, void **pages,
                   const int *nodes, int *status, int flags);