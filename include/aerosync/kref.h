/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/kref.h
 * @brief Kernel Reference Counting
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/atomic.h>
#include <lib/printk.h>

struct kref {
    atomic_t refcount;
};

#define KREF_INIT(n) { .refcount = ATOMIC_INIT(n) }

/**
 * kref_init - initialize object.
 * @kref: object in question.
 */
static inline void kref_init(struct kref *kref)
{
    atomic_set(&kref->refcount, 1);
}

/**
 * kref_get - increment refcount for object.
 * @kref: object.
 */
static inline void kref_get(struct kref *kref)
{
    if (atomic_read(&kref->refcount))
        atomic_inc(&kref->refcount);
    else
        printk(KERN_ERR "kref_get: refcount is 0!\n");
}

/**
 * kref_put - decrement refcount for object.
 * @kref: object.
 * @release: pointer to the function that will clean up the object when the
 *           last reference to the object is released.
 *           This pointer is required, and it is not acceptable to pass kfree
 *           in directly.
 */
static inline int kref_put(struct kref *kref, void (*release)(struct kref *kref))
{
    if (atomic_dec_and_test(&kref->refcount)) {
        release(kref);
        return 1;
    }
    return 0;
}

static inline int kref_read(const struct kref *kref)
{
	return atomic_read(&kref->refcount);
}
