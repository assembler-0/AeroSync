#pragma once

#include <aerosync/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <aerosync/spinlock.h>
#include <aerosync/atomic.h>

/*
 * AeroSync Ultra-High Performance Hybrid Vmalloc Subsystem
 *
 * This system combines:
 * - Linux: vmap_area, vmap_block, augmented RB-tree, lazy purging.
 * - BSD: vmem arenas and per-CPU virtual range caching.
 * - XNU: Submaps and optimized TLB shootdown batching.
 * - AeroSync: NUMA-partitioned address space and lockless RCU lookups.
 */

struct page;
struct vmap_block;

/*
 * vmap_area: Manages a chunk of virtual address space.
 * Augmented RB-tree allows O(log N) search for free gaps.
 */
struct vmap_area {
  unsigned long va_start;
  unsigned long va_end;
  unsigned long flags;
  int nid;

  /* Gap tracking for augmented RB-tree */
  unsigned long rb_max_gap;
  struct rb_node rb_node;

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
#define VMAP_BBMAP_BITS  64
#define VMAP_BLOCK_SIZE  (VMAP_BBMAP_BITS << PAGE_SHIFT)

struct vmap_block {
  spinlock_t lock;
  struct vmap_area *va;
  unsigned long free_map; /* Bitmap of free slots */
  unsigned long dirty_map; /* Bitmap of slots needing purge */
  uint8_t sizes[VMAP_BBMAP_BITS]; /* Sizes of sub-allocations */
  struct list_head list; /* Node in vmap_block_queue */
  int cpu;
  int nid;
  struct rcu_head rcu;
};

/*
 * vmalloc API
 */

void *vmalloc(size_t size);

void *vzalloc(size_t size);

void *vmalloc_node(size_t size, int nid);

void *vmalloc_node_prot(size_t size, int nid, uint64_t pgprot);

void *vmalloc_exec(size_t size);

void *vmalloc_32(size_t size);

void vfree(void *addr);

void vfree_atomic(void *addr);

/* IO / Device Mapping */
void *viomap(uint64_t phys_addr, size_t size);

void *viomap_wc(uint64_t phys_addr, size_t size);

void *viomap_wt(uint64_t phys_addr, size_t size);

void *viomap_wb(uint64_t phys_addr, size_t size);

void *viomap_prot(uint64_t phys_addr, size_t size, uint64_t pgprot);

void viounmap(void *addr);

/* Advanced vmap API */
void *vmap(struct page **pages, unsigned int count, unsigned long flags, uint64_t pgprot);

void vunmap(void *addr);

/* Subsystem Initialization */
void vmalloc_init(void);

void kvmap_purged_init(void);

/* Diagnostics */
void vmalloc_test(void);

void vmalloc_dump(void);
