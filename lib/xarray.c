/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/xarray.c
 * @brief eXtensible Array (Radix Tree) implementation
 * @copyright (C) 2025 assembler-0
 *
 * Implements a 64-ary radix tree with RCU-safe lookups.
 * Based on the Linux XArray concept but implemented clean for AeroSync.
 */

#include <asm-generic/errno-base.h>
#include <linux/xarray.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <kernel/panic.h>
#include <linux/container_of.h>

/*
 * Radix Tree Constants
 * We use a 64-way tree (2^6).
 * 6 bits per level.
 * Level 0: 0 - 63
 * Level 1: 0 - 4095
 * etc.
 */
#define XA_CHUNK_SHIFT  6
#define XA_CHUNK_SIZE   (1UL << XA_CHUNK_SHIFT)
#define XA_CHUNK_MASK   (XA_CHUNK_SIZE - 1)

struct xa_node {
    struct rcu_head rcu;
    uint8_t shift;      /* Bits handled by this node (0 for leaf) */
    uint8_t offset;     /* Slot offset in parent */
    uint8_t count;      /* Number of full slots */
    uint8_t nr_values;  /* Number of value entries */
    struct xa_node *parent;
    void *slots[XA_CHUNK_SIZE];
};

/*
 * Entry Types:
 * - Pointer (aligned 4 bytes): Normal Entry
 * - Value (bit 1 set): Integer/Internal
 * - Sibling (bit 0 set): Multi-index entries (not implemented yet)
 */

static struct xa_node *xa_node_alloc(gfp_t gfp) {
    struct xa_node *node = kmalloc(sizeof(struct xa_node));
    if (node) {
        memset(node, 0, sizeof(*node));
    }
    return node;
}

static void xa_node_free_rcu(struct rcu_head *head) {
    struct xa_node *node = container_of(head, struct xa_node, rcu);
    kfree(node);
}

static void xa_node_free(struct xa_node *node) {
    call_rcu(&node->rcu, xa_node_free_rcu);
}

void xa_init_flags(struct xarray *xa, unsigned int flags) {
    spinlock_init(&xa->xa_lock);
    xa->xa_flags = flags;
    xa->xa_head = NULL;
}

void xa_init(struct xarray *xa) {
    xa_init_flags(xa, 0);
}

/*
 * RCU-safe Lookup
 * Returns NULL if empty, or the entry.
 */
void *xa_load(struct xarray *xa, unsigned long index) {
    rcu_read_lock();

    void *entry = rcu_dereference(xa->xa_head);
    if (!entry) {
        rcu_read_unlock();
        return NULL;
    }

    /* Handle simple case: Single entry at 0? (Not implementing inline head yet) */
    /* Assume head is always a node if shift > 0 */
    struct xa_node *node = (struct xa_node *)entry;

    /*
     * Walk down the tree.
     * Check if index is beyond the current max capacity?
     * Ideally we store the root shift or check the node's shift.
     * For this simplified implementation, we assume head is a node.
     */
    
    /* 
     * CAUTION: In a real radix tree, the root might be a direct entry
     * if the index is 0. Here we enforce node structure for simplicity
     * unless we optimize later.
     */
    if ((uintptr_t)entry & 3) {
        /* Value entry at root? Only possible for index 0 */
        if (index == 0) {
            rcu_read_unlock();
            return entry; /* Strip bits if needed */
        }
        rcu_read_unlock();
        return NULL;
    }

    while (node) {
        if (node->shift == 0) {
            /* Leaf node (contains the actual entries) */
            // The index passed here is absolute. We need to mask it?
            // No, the shift logic below handles descent.
            // Wait, if shift is 0, this IS the leaf array.
            // But we need to index into it.
            // In a radix tree, we shift the index down.
            break;
        }

        uint8_t offset = (index >> node->shift) & XA_CHUNK_MASK;
        void *slot = rcu_dereference(node->slots[offset]);
        
        if (!slot) {
            node = NULL;
            break;
        }

        if ((uintptr_t)slot & 3) {
             /* Found a value/internal entry mid-tree? Should not happen in pure radix */
             rcu_read_unlock();
             return NULL;
        }

        node = (struct xa_node *)slot;
    }
    
    if (node) {
        /* Final level (shift 0) */
        void *val = rcu_dereference(node->slots[index & XA_CHUNK_MASK]);
        rcu_read_unlock();
        return val;
    }

    rcu_read_unlock();
    return NULL;
}

/*
 * Expand the tree upwards to cover the index
 */
static int xa_expand(struct xarray *xa, unsigned long index, gfp_t gfp) {
    struct xa_node *node = xa->xa_head;
    unsigned int shift = 0;

    if (node) {
        shift = node->shift;
        /* Is current root enough? */
        unsigned long max_index = (1UL << (shift + XA_CHUNK_SHIFT)) - 1;
        if (index <= max_index) return 0;
    } else {
        /* Empty tree, create first node for index 0 if needed, or bigger */
        /* For now, just return, store will allocate root */
        return 0;
    }

    while (index > ((1UL << (shift + XA_CHUNK_SHIFT)) - 1)) {
        struct xa_node *new_node = xa_node_alloc(gfp);
        if (!new_node) return -ENOMEM;

        new_node->shift = shift + XA_CHUNK_SHIFT;
        new_node->count = 1;
        new_node->slots[0] = node; /* Old root goes to slot 0 */
        if (node) node->parent = new_node;
        node->offset = 0;

        rcu_assign_pointer(xa->xa_head, new_node);
        node = new_node;
        shift += XA_CHUNK_SHIFT;
    }
    return 0;
}

int xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp) {
    irq_flags_t flags;
    spinlock_lock_irqsave(&xa->xa_lock);

    /* 1. Expand tree if needed */
    if (xa_expand(xa, index, gfp) < 0) {
        spinlock_unlock_irqrestore(&xa->xa_lock, flags);
        return -ENOMEM;
    }

    struct xa_node *node = xa->xa_head;
    
    /* Handle empty tree case */
    if (!node) {
        node = xa_node_alloc(gfp);
        if (!node) {
            spinlock_unlock_irqrestore(&xa->xa_lock, flags);
            return -ENOMEM;
        }
        node->shift = 0; /* Leaf by default */
        
        /* Check if we need levels for this index */
        while (index >= (1UL << (node->shift + XA_CHUNK_SHIFT))) {
             /* This logic should be in expand, but if tree was NULL, expand did nothing. */
             /* Let's fix expand logic or handle it here. 
                Simple: Make expand handle NULL head correctly.
             */
             /* Re-implement simple inline expansion for NULL head: */
             struct xa_node *parent = xa_node_alloc(gfp);
             if (!parent) {
                 kfree(node);
                 spinlock_unlock_irqrestore(&xa->xa_lock, flags);
                 return -ENOMEM;
             }
             parent->shift = node->shift + XA_CHUNK_SHIFT;
             parent->slots[0] = node;
             node->parent = parent;
             node = parent;
        }
        rcu_assign_pointer(xa->xa_head, node);
    }

    /* 2. Walk down, allocating missing nodes */
    while (node->shift > 0) {
        uint8_t offset = (index >> node->shift) & XA_CHUNK_MASK;
        struct xa_node *child = node->slots[offset];

        if (!child) {
            child = xa_node_alloc(gfp);
            if (!child) {
                spinlock_unlock_irqrestore(&xa->xa_lock, flags);
                return -ENOMEM;
            }
            child->shift = node->shift - XA_CHUNK_SHIFT;
            child->parent = node;
            child->offset = offset;
            node->count++;
            
            rcu_assign_pointer(node->slots[offset], child);
        }
        node = child;
    }

    /* 3. Insert into leaf */
    uint8_t offset = index & XA_CHUNK_MASK;
    void *old = node->slots[offset];
    
    if (entry) {
        if (!old) node->count++;
        rcu_assign_pointer(node->slots[offset], entry);
    } else {
        if (old) {
            node->count--;
            rcu_assign_pointer(node->slots[offset], NULL);
            /* TODO: Shrink/Free empty nodes */
        }
    }

    spinlock_unlock_irqrestore(&xa->xa_lock, flags);
    return 0;
}

void *xa_erase(struct xarray *xa, unsigned long index) {
    void *entry = xa_load(xa, index);
    if (entry) {
        xa_store(xa, index, NULL, 0);
    }
    return entry;
}

void xa_destroy_node(struct xa_node *node) {
    if (!node) return;
    if (node->shift > 0) {
        for (int i = 0; i < XA_CHUNK_SIZE; i++) {
            if (node->slots[i]) xa_destroy_node(node->slots[i]);
        }
    }
    kfree(node); /* No RCU needed for destroy */
}

void xa_destroy(struct xarray *xa) {
    struct xa_node *head = xa->xa_head;
    xa->xa_head = NULL;
    xa_destroy_node(head);
}
