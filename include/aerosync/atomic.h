#pragma once

#include <arch/x86_64/atomic.h>

/**
 * @file include/aerosync/atomic.h
 * @brief Generic atomic operations interface
 */

/*
 * atomic_long_t operations - map to atomic_t or atomic64_t based on architecture
 */
#ifdef __x86_64__
typedef atomic64_t atomic_long_t;

#define atomic_long_read(v) atomic64_read(v)
#define atomic_long_set(v, i) atomic64_set(v, i)
#define atomic_long_add(i, v) atomic64_add(i, v)
#define atomic_long_sub(i, v) atomic64_sub(i, v)
#define atomic_long_inc(v) atomic64_inc(v)
#define atomic_long_dec(v) atomic64_dec(v)
#define atomic_long_add_return(i, v) atomic64_add_return(i, v)
#define atomic_long_sub_return(i, v) atomic64_sub_return(i, v)
#define atomic_long_inc_return(v) atomic64_inc_return(v)
#define atomic_long_dec_return(v) atomic64_dec_return(v)
#define atomic_long_xchg(v, n) atomic64_xchg(v, n)
#define atomic_long_cmpxchg(v, o, n) atomic64_cmpxchg(v, o, n)

#else
typedef atomic_t atomic_long_t;

#define atomic_long_read(v) atomic_read(v)
#define atomic_long_set(v, i) atomic_set(v, i)
#define atomic_long_add(i, v) atomic_add(i, v)
#define atomic_long_sub(i, v) atomic_sub(i, v)
#define atomic_long_inc(v) atomic_inc(v)
#define atomic_long_dec(v) atomic_dec(v)
#define atomic_long_xchg(v, n) atomic_xchg(v, n)
#define atomic_long_cmpxchg(v, o, n) atomic_cmpxchg(v, o, n)
#endif

/*
 * Additional helper macros
 */
#define ATOMIC_INIT(i) { (i) }

/**
 * atomic_add_unless - add unless the number is already a given value
 * @v: pointer of type atomic_t
 * @a: the amount to add to v...
 * @u: ...unless v is equal to u.
 *
 * Atomically adds @a to @v, so long as it was not @u.
 * Returns non-zero if the add was done, and zero otherwise.
 */
static inline int atomic_add_unless(atomic_t *v, int a, int u) {
    int c, old;
    c = atomic_read(v);
    while (c != u && (old = atomic_cmpxchg(v, c, c + a)) != c)
        c = old;
    return c != u;
}

#define atomic_inc_not_zero(v) atomic_add_unless((v), 1, 0)
