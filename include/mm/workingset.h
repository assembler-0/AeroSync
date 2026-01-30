/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * AeroSync monolithic kernel
 *
 * @file include/mm/workingset.h
 * @brief Workingset detection for refault tracking
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * Tracks recently evicted pages using shadow entries in the page cache.
 * When a page is evicted, we store a "shadow entry" containing the LRU
 * generation at eviction time. On refault, we compare with the current
 * generation to determine if the page was evicted too soon (thrashing).
 *
 * Based on Linux kernel workingset detection (kernel/mm/workingset.c).
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/atomic.h>

struct folio;
struct pglist_data;
struct vm_object;

/*
 * Shadow entries are stored in the XArray using tagged pointers.
 * We use the low 2 bits to distinguish shadow entries from real folios:
 *   Bit 0 = 1: This is an exceptional entry (ZMM handle OR shadow)
 *   Bit 1 = 1: This is a shadow entry (not ZMM)
 *
 * Layout of a shadow entry value:
 *   [63:16] eviction sequence number
 *   [15:8]  node ID
 *   [7:2]   reserved
 *   [1:0]   0b11 (shadow marker)
 */
#define WORKINGSET_SHADOW_SHIFT     16
#define WORKINGSET_NODE_SHIFT       8
#define WORKINGSET_NODE_MASK        0xFF
#define WORKINGSET_SHADOW_MARKER    0x3  /* bits [1:0] = 0b11 */

/**
 * workingset_is_shadow - Check if an XArray entry is a shadow entry
 * @entry: Pointer from xa_load()
 *
 * Returns true if this is a shadow entry tracking an evicted page.
 */
static inline bool workingset_is_shadow(void *entry) {
  return entry && ((uintptr_t) entry & 0x3) == WORKINGSET_SHADOW_MARKER;
}

/**
 * workingset_pack_shadow - Create a shadow entry
 * @eviction: The LRU generation at eviction time
 * @node: The NUMA node the page was on
 *
 * Returns an opaque value suitable for storing in an XArray.
 */
static inline void *workingset_pack_shadow(unsigned long eviction, int node) {
  unsigned long val = (eviction << WORKINGSET_SHADOW_SHIFT) |
                      ((unsigned long) (node & WORKINGSET_NODE_MASK) << WORKINGSET_NODE_SHIFT) |
                      WORKINGSET_SHADOW_MARKER;
  return (void *) val;
}

/**
 * workingset_unpack_shadow - Extract eviction info from a shadow entry
 * @entry: Shadow entry from XArray
 * @eviction: Output for eviction sequence
 * @node: Output for NUMA node ID
 */
static inline void workingset_unpack_shadow(void *entry, unsigned long *eviction, int *node) {
  unsigned long val = (unsigned long) entry;
  *eviction = val >> WORKINGSET_SHADOW_SHIFT;
  *node = (val >> WORKINGSET_NODE_SHIFT) & WORKINGSET_NODE_MASK;
}

/*
 * Refault distance threshold.
 * If a page is refaulted within this many generations of eviction,
 * it's considered part of the working set and should be activated.
 */
#ifdef CONFIG_MM_WORKINGSET_THRESHOLD
#define WORKINGSET_REFAULT_DISTANCE CONFIG_MM_WORKINGSET_THRESHOLD
#else
#define WORKINGSET_REFAULT_DISTANCE 2
#endif

/**
 * workingset_refault - Handle a refault on a previously evicted page
 * @folio: The newly faulted-in folio
 * @shadow: The shadow entry that was replaced
 *
 * Determines if this refault indicates thrashing and activates the
 * page immediately if so.
 */
void workingset_refault(struct folio *folio, void *shadow);

/**
 * workingset_eviction - Record eviction of a page
 * @folio: The folio being evicted
 * @obj: The vm_object owning this page (for storing shadow)
 *
 * Stores a shadow entry in the page cache to track this eviction.
 * Returns the shadow entry to store, or nullptr if shadows are disabled.
 */
void *workingset_eviction(struct folio *folio, struct vm_object *obj);

/**
 * workingset_activation - Track page activation
 * @folio: The folio being activated
 *
 * Updates statistics for workingset tracking.
 */
void workingset_activation(struct folio *folio);

/**
 * workingset_age_nonresident - Age non-resident (shadow) information
 * @pgdat: The node to age
 *
 * Called periodically to age out old shadow entries and prevent
 * unbounded memory consumption.
 */
void workingset_age_nonresident(struct pglist_data *pgdat);

/**
 * workingset_init - Initialize workingset tracking
 */
void workingset_init(void);

/* Statistics */
struct workingset_stats {
  atomic_long_t refaults; /* Total refaults detected */
  atomic_long_t refault_activate; /* Refaults that caused activation */
  atomic_long_t shadows_stored; /* Shadow entries currently stored */
  atomic_long_t shadows_pruned; /* Shadow entries aged out */
};

extern struct workingset_stats workingset_stats;
