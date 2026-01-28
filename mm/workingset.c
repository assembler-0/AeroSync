/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/workingset.c
 * @brief Workingset detection and refault tracking
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Workingset detection tracks recently evicted pages using shadow entries.
 * When a page is evicted from the LRU, we store a shadow entry containing:
 *   - The LRU generation at eviction time
 *   - The NUMA node the page was on
 *
 * On refault, we compare the current generation with the eviction generation.
 * If the difference is small (within WORKINGSET_REFAULT_DISTANCE), the page
 * was evicted prematurely and should be activated immediately to prevent
 * thrashing.
 *
 * This implementation is inspired by Linux's workingset.c (Johannes Weiner).
 */

#include <mm/workingset.h>
#include <mm/zone.h>
#include <mm/page.h>
#include <mm/vm_object.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

#ifdef CONFIG_MM_WORKINGSET

/* Global statistics */
struct workingset_stats workingset_stats;

/**
 * workingset_eviction - Record eviction of a page
 * @folio: The folio being evicted
 * @obj: The vm_object owning this page
 *
 * Creates a shadow entry encoding the current LRU state at eviction time.
 */
void *workingset_eviction(struct folio *folio, struct vm_object *obj) {
    if (!folio || !obj)
        return NULL;

    int nid = folio->node;
    struct pglist_data *pgdat = node_data[nid];
    if (!pgdat)
        return NULL;

#ifdef CONFIG_MM_MGLRU
    /* For MGLRU: use the current max sequence as eviction timestamp */
    unsigned long eviction = pgdat->lrugen.max_seq;
#else
    /* For standard LRU: use a simple counter (less precise) */
    static atomic_long_t eviction_counter = ATOMIC_INIT(0);
    unsigned long eviction = atomic_long_inc_return(&eviction_counter);
#endif

    void *shadow = workingset_pack_shadow(eviction, nid);
    atomic_long_inc(&workingset_stats.shadows_stored);

    return shadow;
}

/**
 * workingset_refault - Handle a refault on a previously evicted page
 * @folio: The newly faulted-in folio
 * @shadow: The shadow entry that was replaced
 *
 * If the page was evicted recently (within the refault distance),
 * we consider it part of the active working set and activate it
 * immediately instead of putting it on the inactive list.
 */
void workingset_refault(struct folio *folio, void *shadow) {
    if (!workingset_is_shadow(shadow))
        return;

    unsigned long eviction;
    int eviction_node;
    workingset_unpack_shadow(shadow, &eviction, &eviction_node);

    atomic_long_inc(&workingset_stats.refaults);
    atomic_long_dec(&workingset_stats.shadows_stored);

    int nid = folio->node;
    struct pglist_data *pgdat = node_data[nid];
    if (!pgdat)
        return;

#ifdef CONFIG_MM_MGLRU
    unsigned long current_seq = pgdat->lrugen.max_seq;
    unsigned long distance = current_seq - eviction;

    /*
     * If the page was evicted within the refault distance,
     * it's part of the working set and should be activated.
     */
    if (distance <= WORKINGSET_REFAULT_DISTANCE) {
        /*
         * Activate immediately by placing in the youngest generation.
         * The folio_add_lru() will handle the actual list insertion,
         * but we mark it as referenced to boost its position.
         */
        folio->flags |= PG_active | PG_referenced;
        atomic_long_inc(&workingset_stats.refault_activate);
    }
#else
    /*
     * For standard LRU: activate if within threshold.
     * This is less precise but still useful.
     */
    static atomic_long_t current_counter = ATOMIC_INIT(0);
    unsigned long current = atomic_long_read(&current_counter);
    unsigned long distance = current - eviction;

    if (distance <= WORKINGSET_REFAULT_DISTANCE * 1000) {
        folio->flags |= PG_active | PG_referenced;
        atomic_long_inc(&workingset_stats.refault_activate);
    }
#endif
}

/**
 * workingset_activation - Track page activation
 * @folio: The folio being activated
 *
 * Called when a page is promoted from inactive to active.
 * Used for statistics and potential future optimizations.
 */
void workingset_activation(struct folio *folio) {
    /* Currently just tracking - could be extended for adaptive thresholds */
    (void)folio;
}

/**
 * workingset_age_nonresident - Age non-resident (shadow) information
 * @pgdat: The node to age
 *
 * Periodically called to prevent shadow entries from consuming too much
 * memory. We age out shadows that are too old to be useful.
 *
 * In a full implementation, this would walk the radix trees and prune
 * old shadows. For now, we rely on natural replacement during page faults.
 */
void workingset_age_nonresident(struct pglist_data *pgdat) {
    (void)pgdat;
    /*
     * TODO: Implement shadow pruning.
     * This would involve walking vm_objects and removing shadows
     * that are older than a certain threshold.
     *
     * For now, shadows are naturally replaced when pages are faulted
     * back in, which provides implicit aging.
     */
}

void workingset_init(void) {
    atomic_long_set(&workingset_stats.refaults, 0);
    atomic_long_set(&workingset_stats.refault_activate, 0);
    atomic_long_set(&workingset_stats.shadows_stored, 0);
    atomic_long_set(&workingset_stats.shadows_pruned, 0);

    printk(KERN_INFO "workingset: " "Workingset detection initialized (refault distance: %d)\n",
           WORKINGSET_REFAULT_DISTANCE);
}

#else /* !CONFIG_MM_WORKINGSET */

/* Stubs when workingset is disabled */
void *workingset_eviction(struct folio *folio, struct vm_object *obj) {
    (void)folio;
    (void)obj;
    return NULL;
}

void workingset_refault(struct folio *folio, void *shadow) {
    (void)folio;
    (void)shadow;
}

void workingset_activation(struct folio *folio) {
    (void)folio;
}

void workingset_age_nonresident(struct pglist_data *pgdat) {
    (void)pgdat;
}

void workingset_init(void) {
    /* Nothing to do */
}

struct workingset_stats workingset_stats;

#endif /* CONFIG_MM_WORKINGSET */
