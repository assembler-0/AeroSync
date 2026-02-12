#pragma once

#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/smp.h>
#include <linux/list.h>
#include <mm/gfp.h>
#include <mm/page.h>

/* Zone types */
enum zone_type { ZONE_DMA, ZONE_DMA32, ZONE_NORMAL, MAX_NR_ZONES };

/*
 * Page types for Per-CPU lists
 */
enum {
  PCP_LEFT_OVER, /* Pages that didn't fit into other lists */
  PCP_HOT,       /* Hot pages (recently freed) */
  PCP_COLD,      /* Cold pages */
  PCP_TYPES
};

struct pglist_data;

/*
 * Migration types for fragmentation control
 */
enum migrate_type {
  MIGRATE_UNMOVABLE,
  MIGRATE_RECLAIMABLE,
  MIGRATE_MOVABLE,
  MIGRATE_PCPTYPES,
#ifdef CONFIG_MM_PMM_HIGHATOMIC
  MIGRATE_HIGHATOMIC,
#endif
#ifdef CONFIG_MM_PMM_CMA
  MIGRATE_CMA,
#endif
  MIGRATE_ISOLATE,
  MIGRATE_TYPES
};

#ifndef CONFIG_MM_PMM_PAGEBLOCK_ORDER
#define PAGEBLOCK_ORDER 9
#else
#define PAGEBLOCK_ORDER CONFIG_MM_PMM_PAGEBLOCK_ORDER
#endif

#define PAGEBLOCK_NR_PAGES (1UL << PAGEBLOCK_ORDER)

struct free_area {
  struct list_head free_list[MIGRATE_TYPES];
  unsigned long nr_free;
};

#ifndef CONFIG_MM_PMM_PCP_MAX_ORDER
#define PCP_ORDERS 4
#else
#define PCP_ORDERS (CONFIG_MM_PMM_PCP_MAX_ORDER + 1)
#endif

#ifdef CONFIG_MM_PMM_PCP_HOT_COLD
#define PCP_LIST_HOT 0
#define PCP_LIST_COLD 1
#define PCP_LISTS 2
#else
#define PCP_LISTS 1
#endif

struct per_cpu_pages {
  int count;
  int high;
  int batch;
#ifdef CONFIG_MM_PMM_PCP_DYNAMIC
  int high_min;
  int high_max;
  int batch_min;
  int batch_max;
#endif
#ifdef CONFIG_MM_PMM_PCP_CACHE_COLORING
  int color;
  int color_mask;
#endif
  struct list_head lists[PCP_ORDERS][PCP_LISTS];
#ifdef CONFIG_MM_PMM_STATS
  atomic_long_t alloc_count;
  atomic_long_t free_count;
  atomic_long_t refill_count;
  atomic_long_t drain_count;
#endif
#ifdef CONFIG_MM_PMM_STALL_TRACKING
  atomic_long_t stall_count;
  atomic_long_t stall_time_ns;
#endif
};

#ifdef CONFIG_MM_PMM_INLINE_HOTPATH
#define PMM_INLINE __always_inline
#else
#define PMM_INLINE inline
#endif

alignas(64) struct zone {
  /* Cache line 0: Hot allocation path (read-write) */
  spinlock_t lock;
  unsigned long nr_free_pages;
  unsigned int max_free_order;

#ifdef CONFIG_MM_PMM_BITMAP_TRACKING
  unsigned long free_area_bitmap[MAX_ORDER];
#endif

  /* Cache line 1: Free area management */
  struct free_area free_area[MAX_ORDER];

  /* Cache line 2+: Per-CPU page frame cache */
  struct per_cpu_pages pageset[MAX_CPUS];

  /* Read-mostly fields */
  unsigned long zone_start_pfn;
  unsigned long spanned_pages;
  unsigned long present_pages;
  const char *name;
  struct pglist_data *zone_pgdat;

  /* Watermarks (read-mostly) */
  unsigned long watermark[4];
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST
  unsigned long watermark_boost;
#ifdef CONFIG_MM_PMM_WATERMARK_BOOST_DECAY
  unsigned long watermark_boost_factor;
  uint64_t last_boost_decay_time;
#endif
#endif

#ifdef CONFIG_MM_PMM_PAGEBLOCK_METADATA
  unsigned long *pageblock_flags;
#endif

#ifdef CONFIG_MM_PMM_DEFERRED_COALESCING
  struct list_head deferred_list;
  unsigned int deferred_count;
#endif

#ifdef CONFIG_MM_PMM_DIRTY_TRACKING
  atomic_long_t nr_dirty;
  atomic_long_t dirty_exceeded_count;
#endif

#ifdef CONFIG_MM_PMM_FRAGMENTATION_INDEX
  unsigned int fragmentation_index;
  uint64_t last_frag_calc_time;
#endif

#ifdef CONFIG_MM_PMM_HIGHATOMIC
  unsigned long nr_reserved_highatomic;
#endif

#ifdef CONFIG_MM_PMM_MIGRATION_TRACKING
  atomic_long_t pageblock_steal_count;
  atomic_long_t migration_type_fallback[MIGRATE_TYPES];
#endif

#ifdef CONFIG_MM_PMM_COMPACTION_DEFER
  unsigned int compact_defer_shift;
  unsigned int compact_considered;
  unsigned int compact_order_failed;
#endif

  /* Stats */
#ifdef CONFIG_MM_PMM_STATS
  atomic_long_t alloc_success;
  atomic_long_t alloc_fail;
  atomic_long_t reclaim_success;
  atomic_long_t fallback_count;
  atomic_long_t steal_count;
#ifdef CONFIG_MM_PMM_STATS_LATENCY
  atomic_long_t alloc_latency_ns[MAX_ORDER];
  atomic_long_t alloc_latency_count[MAX_ORDER];
#endif
#ifdef CONFIG_MM_PMM_STALL_TRACKING
  atomic_long_t direct_reclaim_stalls;
  atomic_long_t kswapd_wakeups[3]; /* low, high, critical */
#endif
#endif
  atomic_long_t vm_stat[32];
};

#ifndef MAX_NUMNODES
#ifndef CONFIG_MAX_NUMNODES
#define MAX_NUMNODES 8
#else
#define MAX_NUMNODES CONFIG_MAX_NUMNODES
#endif
#endif
#define NUMA_NO_NODE (-1)

#define MAX_ZONES_PER_ZONELIST (MAX_NUMNODES * MAX_NR_ZONES + 1)

struct zonelist {
  struct zone *_zones[MAX_ZONES_PER_ZONELIST];
};

#define MAX_NR_GENS 4

struct lrugen {
  /* [generation][anon/file] */
  struct list_head lists[MAX_NR_GENS][2];
  atomic_long_t nr_pages[MAX_NR_GENS][2];

  unsigned long max_seq;
  unsigned long min_seq[2]; /* [0] = file, [1] = anon */

  /* Generation walk state */
  atomic_t gen_counter;
};

alignas(64) struct pglist_data {
  struct zone node_zones[MAX_NR_ZONES];
  struct zonelist node_zonelists[MAX_NR_ZONES];

  unsigned long node_start_pfn;
  unsigned long node_present_pages;
  unsigned long node_spanned_pages;
  int node_id;

  wait_queue_head_t kswapd_wait;
  struct task_struct *kswapd_task;

  /* LRU Management */
  spinlock_t lru_lock;
  struct lrugen lrugen;
};

extern struct pglist_data *node_data[MAX_NUMNODES];

/* Global array of zones - kept for compatibility with non-NUMA paths */
extern struct zone managed_zones[MAX_NR_ZONES];

/*
 * Zone watermarks
 */
#define WMARK_MIN 0
#define WMARK_LOW 1
#define WMARK_HIGH 2
#define WMARK_PROMO 3

/*
 * Prototypes
 */
void free_area_init(void);
void pmm_verify(void);
void build_all_zonelists(void);

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order);
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order);

int rmqueue_bulk(struct zone *zone, unsigned int order, unsigned int count,
                 struct list_head *list, int migratetype);

void free_pcp_pages(struct zone *zone, int count, struct list_head *list,
                    int order);

void __free_pages(struct page *page, unsigned int order);

/* Boot-only: bypasses poisoning, PCP, locking. Single-threaded init only. */
void __free_pages_boot_core(struct page *page, unsigned int order);

void free_pages(uint64_t addr, unsigned int order);

static inline struct folio *alloc_page(gfp_t gfp_mask) {
  return alloc_pages(gfp_mask, 0);
}

int cpu_to_node(int cpu);
static inline int this_node(void) { return cpu_to_node((int)smp_get_id()); }

static inline void __free_page(struct page *page) { __free_pages(page, 0); }
