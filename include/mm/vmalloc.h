#pragma once

#include <aerosync/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/maple_tree.h>
#include <aerosync/spinlock.h>
#include <aerosync/errno.h>

/*
 * AeroSync High Performance Hybrid vmalloc Subsystem
 *
 * This system combines:
 * - Linux: vmap_area, vmap_block, augmented RB-tree, lazy purging.
 * - BSD: vmem arenas and per-CPU virtual range caching.
 * - XNU: Submaps and optimized TLB shootdown batching.
 * - AeroSync: NUMA-partitioned address space and lockless RCU lookups.
 *
 * Key optimizations (v2):
 * - Maple Tree for O(1) gap finding (CONFIG_VMALLOC_MAPLE_TREE)
 * - Lock-free fast path with per-CPU caching (CONFIG_VMALLOC_LOCKLESS_FAST_PATH)
 * - NUMA-partitioned address space (CONFIG_VMALLOC_NUMA_PARTITION)
 * - Batched TLB shootdown with unified flush (CONFIG_VMALLOC_LAZY_FLUSH)
 */

struct page;
struct vmap_block;

/* ========================================================================
 * Configuration Defaults (overridden by Kconfig)
 * ======================================================================= */

#ifndef CONFIG_VMALLOC_PCP_BIN_COUNT
#define CONFIG_VMALLOC_PCP_BIN_COUNT 8
#endif

#ifndef CONFIG_VMALLOC_PCP_BIN_THRESHOLD
#define CONFIG_VMALLOC_PCP_BIN_THRESHOLD 64
#endif

#ifndef CONFIG_VMALLOC_PCP_BATCH_SIZE
#define CONFIG_VMALLOC_PCP_BATCH_SIZE 16
#endif

#ifndef CONFIG_VMALLOC_LAZY_THRESHOLD_MB
#define CONFIG_VMALLOC_LAZY_THRESHOLD_MB 32
#endif

#ifndef CONFIG_VMALLOC_LAZY_TIMEOUT_MS
#define CONFIG_VMALLOC_LAZY_TIMEOUT_MS 100
#endif

/* ========================================================================
 * Per-CPU Cache Configuration
 * ======================================================================= */

#define VMALLOC_PCP_BINS        CONFIG_VMALLOC_PCP_BIN_COUNT
#define VMALLOC_PCP_THRESHOLD   CONFIG_VMALLOC_PCP_BIN_THRESHOLD
#define VMALLOC_PCP_BATCH       CONFIG_VMALLOC_PCP_BATCH_SIZE

/* Size bin mapping: bin N handles allocations of (1 << N) pages */
static inline int vmalloc_size_to_bin(unsigned long pages) {
  if (pages == 0) return -EINVAL;
  if (pages > (1UL << (VMALLOC_PCP_BINS - 1))) return -EINVAL;

  /* Find the smallest bin that fits this allocation */
  int bin = 0;
  unsigned long bin_size = 1;
  while (bin_size < pages && bin < VMALLOC_PCP_BINS - 1) {
    bin++;
    bin_size <<= 1;
  }
  return bin;
}

static inline unsigned long vmalloc_bin_to_pages(int bin) {
  if (bin < 0 || bin >= VMALLOC_PCP_BINS) return 0;
  return 1UL << bin;
}

/* ========================================================================
 * Lazy Flush Configuration
 * ======================================================================= */

#define VM_LAZY_FREE_THRESHOLD  ((unsigned long)CONFIG_VMALLOC_LAZY_THRESHOLD_MB << 20)
#define VM_LAZY_TIMEOUT_NS      ((uint64_t)CONFIG_VMALLOC_LAZY_TIMEOUT_MS * 1000000ULL)

/* ========================================================================
 * NUMA Partitioning
 * ======================================================================= */

#ifdef CONFIG_VMALLOC_NUMA_PARTITION
#define VMALLOC_NUMA_PARTITIONED 1
#else
#define VMALLOC_NUMA_PARTITIONED 0
#endif

/*
 * vmap_area: Manages a chunk of virtual address space.
 * With CONFIG_VMALLOC_MAPLE_TREE, uses Maple Tree for O(1) gap finding.
 * Legacy RB-tree support retained for fallback.
 */
struct vmap_area {
  unsigned long va_start;
  unsigned long va_end;
  unsigned long flags;
  int nid;

#ifdef CONFIG_VMALLOC_MAPLE_TREE
  /* Maple tree doesn't need embedded nodes - uses external indexing */
#else
  /* Gap tracking for augmented RB-tree (legacy) */
  unsigned long rb_max_gap;
  struct rb_node rb_node;
#endif

  struct list_head list; /* Node/Global list */

  union {
    struct {
      struct list_head purge_list;
      struct rcu_head rcu;
    };

    struct vmap_block *vb; /* If managed by vmap_block */
  };
};

/* vmap_area flags */
#define VMAP_AREA_USED     0x01
#define VMAP_AREA_LAZY     0x02  /* Pending purge */
#define VMAP_AREA_STATIC   0x04  /* Boot/Fixed mapping (MMIO) */
#define VMAP_AREA_BLOCK    0x08  /* Managed by vmap_block */
#define VMAP_AREA_PCP      0x10  /* Currently in Per-CPU cache */
#define VMAP_AREA_EXTERNAL 0x20  /* External RAM pages (don't free in vunmap) */

/*
 * vmap_block: Sub-allocator for small virtual address ranges.
 * Reduces global lock contention by using per-CPU caches.
 */
#ifdef CONFIG_VMALLOC_LARGE_BLOCKS
#define VMAP_BBMAP_BITS  256
#else
#define VMAP_BBMAP_BITS  64
#endif

#define VMAP_BLOCK_SIZE  (VMAP_BBMAP_BITS << PAGE_SHIFT)

struct vmap_block {
  spinlock_t lock;
  struct vmap_area *va;
  unsigned long free_map[VMAP_BBMAP_BITS / 64]; /* Bitmap of free slots */
  unsigned long dirty_map[VMAP_BBMAP_BITS / 64]; /* Bitmap of slots needing purge */
  uint8_t sizes[VMAP_BBMAP_BITS]; /* Sizes of sub-allocations */
  struct list_head list; /* Node in vmap_block_queue */
  int cpu;
  int nid;
  int free_count; /* Fast check for available space */
  struct rcu_head rcu;
};

#ifdef CONFIG_VMALLOC_BLOCK_CLASSES
/*
 * vmap_block size classes for different allocation patterns.
 * Reduces internal fragmentation.
 */
struct vmap_block_class {
  int min_pages; /* Minimum allocation this class handles */
  int max_pages; /* Maximum allocation this class handles */
  int block_pages; /* Block size for this class */
};

#define VMAP_BLOCK_CLASSES 3

extern const struct vmap_block_class vmap_block_classes[VMAP_BLOCK_CLASSES];
#endif /* CONFIG_VMALLOC_BLOCK_CLASSES */

/*
 * vmap_node: Per-NUMA node vmalloc management.
 * With Maple Tree, provides O(1) gap finding.
 */
alignas(64) struct vmap_node {
#ifdef CONFIG_VMALLOC_MAPLE_TREE
  struct maple_tree va_mt; /* Maple tree for address management */
#else
  struct rb_root root; /* Legacy RB-tree */
#endif
  spinlock_t lock;
  struct list_head list;
  struct list_head purge_list;
  atomic_long_t nr_purged;

#ifdef CONFIG_VMALLOC_NUMA_PARTITION
  unsigned long va_start; /* Start of this node's VA region */
  unsigned long va_end; /* End of this node's VA region */
#endif

  uint64_t last_flush_time; /* For lazy flush timeout */
  int nid;
};

/*
 * vmap_pcp: Enhanced per-CPU virtual address cache.
 * Multiple size bins for better hit rate, batch refill for efficiency.
 */
alignas(64) struct vmap_pcp {
  spinlock_t lock;

  /* Size-class bins: bin[i] holds ranges of (1 << i) pages */
  struct list_head bins[VMALLOC_PCP_BINS];
  int bin_count[VMALLOC_PCP_BINS];

  /* Metadata cache for fast allocation */
  struct list_head free_va;
  int nr_va;

  /* Statistics (debug) */
#ifdef CONFIG_MM_HARDENING
  unsigned long hits;
  unsigned long misses;
  unsigned long refills;
#endif
};

/*
 * vmap_block_queue: Per-CPU queue of vmap_blocks with free space.
 */
struct vmap_block_queue {
  spinlock_t lock;
  struct list_head free;
#ifdef CONFIG_VMALLOC_BLOCK_CLASSES
  struct list_head class_free[VMAP_BLOCK_CLASSES];
#endif
};

/*
 * vmalloc API
 */

void *vmalloc(size_t size);

void *vzalloc(size_t size);

void *vmalloc_node(size_t size, int nid);

void *vmalloc_node_prot(size_t size, int nid, uint64_t pgprot);

void *vmalloc_node_stack(size_t size, int nid);

int vmalloc_bulk_stacks(int count, int node, void **stacks);

static inline void *vmalloc_stack(size_t size) { return vmalloc_node_stack(size, -1); }

void *vmalloc_exec(size_t size);

void *vmalloc_32(size_t size);

void vfree(void *addr);

/* placeholder */
void vfree_atomic(void *addr);

/* IO / Device Mapping */
void *ioremap(uint64_t phys_addr, size_t size);

void *ioremap_wc(uint64_t phys_addr, size_t size);

void *ioremap_wt(uint64_t phys_addr, size_t size);

void *ioremap_wb(uint64_t phys_addr, size_t size);

void *ioremap_prot(uint64_t phys_addr, size_t size, uint64_t pgprot);

void iounmap(void *addr);

#define ioremap_uc(pa, s) ioremap(pa, s)

/* Advanced vmap API */
void *vmap(struct page **pages, unsigned int count, unsigned long flags, uint64_t pgprot);

void vunmap(void *addr);

/* Subsystem Initialization */
void vmalloc_init(void);

void kvmap_purged_init(void);

/* Diagnostics */
void vmalloc_test(void);

void vmalloc_dump(void);

/* Internal APIs for testing */
#ifdef CONFIG_MM_HARDENING
void vmalloc_pcp_stats(int cpu, unsigned long *hits, unsigned long *misses);
#endif
