/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * AeroSync monolithic kernel
 *
 * @file include/mm/swap.h
 * @brief Swap subsystem definitions
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * The swap subsystem provides secondary storage for anonymous pages
 * when memory pressure exceeds ZMM compression capacity or when ZMM
 * compression ratios are poor.
 *
 * Architecture:
 * ─────────────
 *   1. Swap slots are allocated from swap devices in clusters
 *   2. Per-CPU slot caches reduce contention
 *   3. Swap cache provides swapin readahead
 *   4. Swap entries are encoded in PTEs when pages are swapped out
 *
 * Based on Linux kernel swap implementation.
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/atomic.h>
#include <aerosync/rw_semaphore.h>
#include <mm/page.h>

#include "gfp.h"

struct folio;
struct file;
struct block_device;

/*
 * Swap entry encoding:
 * We encode swap entries in a format that fits in unused PTE bits.
 *
 * Layout (64-bit):
 *   [63]     = 0 (not present)
 *   [62:58]  = swap type (device index, max 32 devices)
 *   [57:1]   = swap offset (slot number)
 *   [0]      = 0 (distinguishes from valid PTE)
 *
 * This allows ~128TB per swap device with 4KB pages.
 */
typedef struct {
  unsigned long val;
} swp_entry_t;

#define SWP_TYPE_SHIFT      58
#define SWP_TYPE_MASK       0x1F
#define SWP_OFFSET_SHIFT    1
#define SWP_OFFSET_MASK     ((1UL << 57) - 1)

#define MAX_SWAPFILES       32

/* Swap entry constructors and accessors */
static inline swp_entry_t swp_entry(unsigned int type, unsigned long offset) {
  swp_entry_t entry;
  entry.val = ((unsigned long) type << SWP_TYPE_SHIFT) |
              (offset << SWP_OFFSET_SHIFT);
  return entry;
}

static inline unsigned int swp_type(swp_entry_t entry) {
  return (entry.val >> SWP_TYPE_SHIFT) & SWP_TYPE_MASK;
}

static inline unsigned long swp_offset(swp_entry_t entry) {
  return (entry.val >> SWP_OFFSET_SHIFT) & SWP_OFFSET_MASK;
}

static inline bool non_swap_entry(swp_entry_t entry) {
  return entry.val == 0;
}

/*
 * Swap cluster for contiguous allocation.
 * Clusters improve sequential I/O and reduce fragmentation.
 */
#define SWAP_CLUSTER_SHIFT  8
#define SWAP_CLUSTER_SIZE   (1 << SWAP_CLUSTER_SHIFT)  /* 256 slots per cluster */

struct swap_cluster_info {
  spinlock_t lock;
  unsigned int count; /* Free slots in this cluster */
  unsigned int flags;
#define CLUSTER_FLAG_FREE   0x01
#define CLUSTER_FLAG_FULL   0x02
  struct list_head list; /* In free/partial/full list */
};

/*
 * Per-CPU swap slot cache for lock-free allocation in the fast path.
 */
struct swap_slots_cache {
  unsigned long *slots; /* Array of swap offsets */
  unsigned int nr; /* Number of cached slots */
  unsigned int max; /* Maximum slots to cache */
  spinlock_t lock;
};

/*
 * struct swap_info_struct - Describes a swap device/file
 */
struct swap_info_struct {
  unsigned long flags; /* SWP_* flags */
#define SWP_USED        (1 << 0)    /* In use */
#define SWP_WRITEOK     (1 << 1)    /* Ready for writes */
#define SWP_DISCARDABLE (1 << 2)    /* Supports TRIM/discard */
#define SWP_DISCARDING  (1 << 3)    /* Discard in progress */
#define SWP_SOLIDSTATE  (1 << 4)    /* SSD-backed */
#define SWP_FILE        (1 << 5)    /* Swap file (not partition) */
#define SWP_SYNTHETIC   (1 << 6)    /* Synthetic swap for testing */

  int prio; /* Swap priority (higher = preferred) */
  int type; /* Index in swap_info[] */

  struct file *swap_file; /* Backing file (if SWP_FILE) */
  struct block_device *bdev; /* Backing block device */

  /* Cluster allocator */
  struct swap_cluster_info *cluster_info;
  unsigned int cluster_nr; /* Total clusters */
  unsigned int cluster_next; /* Hint for next allocation */
  struct list_head free_clusters;
  struct list_head partial_clusters;

  /* Per-slot reference counts */
  unsigned char *swap_map; /* Per-slot refcount (0=free, 255=bad) */
  unsigned long highest_bit; /* Highest valid slot */
  unsigned long lowest_bit; /* Lowest valid slot */

  /* Extent mapping (for files) */
  struct list_head extent_list;

  /* Per-CPU slot caches */
  struct swap_slots_cache __percpu *slots_cache;

  /* Statistics */
  atomic_long_t inuse_pages;
  atomic_long_t total_pages;

  /* Locking */
  spinlock_t lock;
  struct rw_semaphore alloc_lock; /* Serializes swapon/swapoff */

  char name[64]; /* Device/file path */
};

/* Swap map special values */
#define SWAP_MAP_FREE       0       /* Slot is free */
#define SWAP_MAP_MAX        0xFE    /* Maximum reference count */
#define SWAP_MAP_BAD        0xFF    /* Slot is unusable (bad sector) */

/* Global swap state */
extern struct swap_info_struct *swap_info[MAX_SWAPFILES];
extern int nr_swapfiles;
extern atomic_long_t total_swap_pages;
extern atomic_long_t nr_swap_pages; /* Free swap pages */

/*
 * Swap cache:
 * Pages being swapped in/out are temporarily cached to handle
 * concurrent faults and enable readahead.
 */
struct swap_cache_entry {
  swp_entry_t entry;
  struct folio *folio;
  struct list_head list;
  atomic_t refcount;
};

/* Core swap operations */
int swap_init(void);

int sys_swapon(const char *path, int flags);

int sys_swapoff(const char *path);

/* Slot allocation */
swp_entry_t get_swap_page(struct folio *folio);

void swap_free(swp_entry_t entry);

int swap_duplicate(swp_entry_t entry);

/* Swap I/O */
int swap_writepage(struct folio *folio, swp_entry_t entry);

struct folio *swap_readpage(swp_entry_t entry);

/* Swap cache */
struct folio *lookup_swap_cache(swp_entry_t entry);

int add_to_swap_cache(struct folio *folio, swp_entry_t entry);

void delete_from_swap_cache(struct folio *folio);

/* Swap readahead */
struct folio *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask);

/* Helpers */
static inline bool swap_is_enabled(void) {
  return nr_swapfiles > 0 && atomic_long_read(&nr_swap_pages) > 0;
}

/**
 * folio_swapped - Check if a folio is backed by swap
 */
static inline bool folio_swapped(struct folio *folio) {
  return folio && (folio->flags & (1UL << 20)); /* PG_swapcache */
}

/* PTE/swap entry conversion */
#define PTE_SWAP_MARKER     0x8000000000000000UL  /* Bit 63 = swap marker */

static inline bool pte_is_swap(uint64_t pte) {
  return (pte & PTE_SWAP_MARKER) && !(pte & 0x1); /* Marker set, not present */
}

static inline swp_entry_t pte_to_swp_entry(uint64_t pte) {
  swp_entry_t entry;
  entry.val = pte & ~PTE_SWAP_MARKER;
  return entry;
}

static inline uint64_t swp_entry_to_pte(swp_entry_t entry) {
  return entry.val | PTE_SWAP_MARKER;
}
