#pragma once

#include <kernel/spinlock.h>
#include <linux/rcupdate.h>
#include <mm/gfp.h>

/* XArray flags */
#define XA_FLAGS_LOCK_IRQ   1
#define XA_FLAGS_LOCK_BH    2
#define XA_FLAGS_ALLOC      4 /* Allocate Entry */

/**
 * struct xarray - The anchor of the array.
 * @xa_lock: Spinlock protecting updates.
 * @xa_flags: Flags (locking behavior).
 * @xa_head: Root of the radix tree (RCU protected).
 */
struct xarray {
    spinlock_t xa_lock;
    unsigned int xa_flags;
    void *xa_head;
};

/* Initialization */
#define XA_INIT(name, flags) { \
    .xa_lock = SPINLOCK_INIT, \
    .xa_flags = flags, \
    .xa_head = NULL, \
}

void xa_init(struct xarray *xa);
void xa_init_flags(struct xarray *xa, unsigned int flags);

/* Operations */
void *xa_load(struct xarray *xa, unsigned long index);
int xa_store(struct xarray *xa, unsigned long index, void *entry, gfp_t gfp);
void *xa_erase(struct xarray *xa, unsigned long index);
void xa_destroy(struct xarray *xa);

/* Advanced */
struct xa_limit {
    unsigned int min;
    unsigned int max;
};
int xa_alloc(struct xarray *xa, uint32_t *id, void *entry, struct xa_limit limit, gfp_t gfp);

/* Iteration */
#define xa_for_each(xa, entry, index) \
    for (index = 0, entry = xa_load(xa, index); \
         entry != NULL; \
         index++, entry = xa_load(xa, index)) \
         /* Note: Optimised iterator needed for performance later */

/* Internal Helper for NULL checks */
static inline bool xa_is_err(const void *entry) {
    return (uintptr_t)entry >= (uintptr_t)-4095;
}
