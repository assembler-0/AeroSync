#pragma once

#include <aerosync/spinlock.h>
#include <aerosync/atomic.h>
#include <linux/list.h>
#include <mm/page.h>
#include <arch/x86_64/cpu.h>
#include <mm/gfp.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/smp.h>
#include <aerosync/wait.h>

/* Zone types */
enum zone_type {
  ZONE_DMA,
  ZONE_DMA32,
  ZONE_NORMAL,
  MAX_NR_ZONES
};

/*
 * Page types for Per-CPU lists
 */
enum {
  PCP_LEFT_OVER, /* Pages that didn't fit into other lists */
  PCP_HOT, /* Hot pages (recently freed) */
  PCP_COLD, /* Cold pages */
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
  MIGRATE_PCPTYPES, /* Special for PCP */
  MIGRATE_TYPES = 4
};

struct free_area {
  struct list_head free_list[MIGRATE_TYPES];
  unsigned long nr_free;
};

#define PCP_ORDERS 4

struct per_cpu_pages {
  int count; /* total number of pages in all lists */
  int high; /* high watermark, emptying needed */
  int batch; /* chunk size for buddy add/remove */
  struct list_head lists[PCP_ORDERS]; /* lists of pages for each order */
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

  /* Watermarks */
  unsigned long watermark[3]; /* MIN, LOW, HIGH */
  unsigned long nr_free_pages;
  unsigned int max_free_order; /* Highest order with at least one free block */

  const char *name;

  /* Stats */
  atomic_long_t vm_stat[32]; // NR_FREE_PAGES, etc.

  struct pglist_data *zone_pgdat;
}
    __aligned(64); // Cache line aligned

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

struct pglist_data {
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
} __aligned(64);

extern struct pglist_data *node_data[MAX_NUMNODES];

/* Global array of zones - kept for compatibility with non-NUMA paths */
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
void pmm_verify(void);
void build_all_zonelists(void);

struct folio *alloc_pages(gfp_t gfp_mask, unsigned int order);
struct folio *alloc_pages_node(int nid, gfp_t gfp_mask, unsigned int order);

int rmqueue_bulk(struct zone *zone, unsigned int order, unsigned int count,
                 struct list_head *list, int migratetype);

void free_pcp_pages(struct zone *zone, int count, struct list_head *list, int order);

void __free_pages(struct page *page, unsigned int order);

void free_pages(uint64_t addr, unsigned int order);

static inline struct folio *alloc_page(gfp_t gfp_mask) {
  return alloc_pages(gfp_mask, 0);
}

int cpu_to_node(int cpu);
static inline int this_node(void) {
    return cpu_to_node((int)smp_get_id());
}

static inline void __free_page(struct page *page) {
  __free_pages(page, 0);
}
