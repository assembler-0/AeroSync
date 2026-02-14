#pragma once

#include <aerosync/types.h>
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

static __always_inline bool atomic_try_cmpxchg(atomic_t *v, int *old, int new) {
    return try_cmpxchg(&v->counter, old, new);
}

static __always_inline bool atomic64_try_cmpxchg(atomic64_t *v, long *old, long new) {
    return try_cmpxchg64(&v->counter, old, new);
}

#define atomic_long_try_cmpxchg(v, o, n) atomic64_try_cmpxchg(v, o, n)

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

static __always_inline bool atomic_long_try_cmpxchg(atomic_long_t *v, long *old, long new) {
    return try_cmpxchg(&v->counter, (int *)old, (int)new);
}
#endif

/*
 * Linux Compatibility Layer (ATOMIC_LINUX_COMPAT)
 */
#ifdef ATOMIC_LINUX_COMPAT
#define atomic_set_release(v, i) atomic_set(v, i)
#define atomic_read_acquire(v) atomic_read(v)

#define atomic_add_return_relaxed(i, v) atomic_add_return(i, v)
#define atomic_add_return_acquire(i, v) atomic_add_return(i, v)
#define atomic_add_return_release(i, v) atomic_add_return(i, v)

#define atomic_sub_return_relaxed(i, v) atomic_sub_return(i, v)
#define atomic_sub_return_acquire(i, v) atomic_sub_return(i, v)
#define atomic_sub_return_release(i, v) atomic_sub_return(i, v)

#define atomic_inc_return_relaxed(v) atomic_inc_return(v)
#define atomic_dec_return_relaxed(v) atomic_dec_return(v)

#define atomic_cmpxchg_relaxed(v, o, n) atomic_cmpxchg(v, o, n)
#define atomic_cmpxchg_acquire(v, o, n) atomic_cmpxchg(v, o, n)
#define atomic_cmpxchg_release(v, o, n) atomic_cmpxchg(v, o, n)

#define atomic_try_cmpxchg_relaxed(v, o, n) atomic_try_cmpxchg(v, o, n)
#define atomic_try_cmpxchg_acquire(v, o, n) atomic_try_cmpxchg(v, o, n)
#define atomic_try_cmpxchg_release(v, o, n) atomic_try_cmpxchg(v, o, n)

#define atomic64_try_cmpxchg_relaxed(v, o, n) atomic64_try_cmpxchg(v, o, n)
#endif

/*
 * Additional helper macros
 */
#define ATOMIC_INIT(i) { (i) }
#define ATOMIC_LONG_INIT(i) { (i) }

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
