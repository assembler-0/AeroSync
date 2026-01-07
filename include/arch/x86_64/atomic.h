#pragma once

#include <kernel/types.h>
#include <compiler.h>

/**
 * @file include/arch/x86_64/atomic.h
 * @brief Optimized x86_64 atomic operations (Linux-style)
 */

typedef struct {
    volatile int counter;
} atomic_t;

typedef struct {
    volatile long counter;
} atomic64_t;

/*
 * Atomic Integer Operations
 */

static __always_inline int atomic_read(const atomic_t *v) {
    return READ_ONCE(v->counter);
}

static __always_inline void atomic_set(atomic_t *v, int i) {
    WRITE_ONCE(v->counter, i);
}

static __always_inline void atomic_add(int i, atomic_t *v) {
    asm volatile("lock; addl %1,%0"
                 : "+m" (v->counter)
                 : "ir" (i) : "memory");
}

static __always_inline void atomic_sub(int i, atomic_t *v) {
    asm volatile("lock; subl %1,%0"
                 : "+m" (v->counter)
                 : "ir" (i) : "memory");
}

static __always_inline int atomic_add_return(int i, atomic_t *v) {
    int __i = i;
    asm volatile("lock; xaddl %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i + __i;
}

static __always_inline int atomic_sub_return(int i, atomic_t *v) {
    return atomic_add_return(-i, v);
}

#define atomic_inc_return(v)  (atomic_add_return(1, v))
#define atomic_dec_return(v)  (atomic_sub_return(1, v))

static __always_inline bool atomic_sub_and_test(int i, atomic_t *v) {
    bool c;
    asm volatile("lock; subl %2,%0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : "ir" (i) : "memory");
    return c;
}

static __always_inline void atomic_inc(atomic_t *v) {
    asm volatile("lock; incl %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline void atomic_dec(atomic_t *v) {
    asm volatile("lock; decl %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline bool atomic_dec_and_test(atomic_t *v) {
    bool c;
    asm volatile("lock; decl %0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : : "memory");
    return c;
}

static __always_inline bool atomic_inc_and_test(atomic_t *v) {
    bool c;
    asm volatile("lock; incl %0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : : "memory");
    return c;
}

static __always_inline int atomic_xchg(atomic_t *v, int new) {
    int ret = new;
    asm volatile("xchgl %0, %1"
                 : "+r" (ret), "+m" (v->counter)
                 : : "memory");
    return ret;
}

static __always_inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    int ret;
    asm volatile("lock; cmpxchgl %2, %1"
                 : "=a" (ret), "+m" (v->counter)
                 : "r" (new), "0" (old)
                 : "memory");
    return ret;
}

/*
 * Atomic 64-bit Operations
 */

static __always_inline long atomic64_read(const atomic64_t *v) {
    return READ_ONCE(v->counter);
}

static __always_inline void atomic64_set(atomic64_t *v, long i) {
    WRITE_ONCE(v->counter, i);
}

static __always_inline void atomic64_add(long i, atomic64_t *v) {
    asm volatile("lock; addq %1,%0"
                 : "+m" (v->counter)
                 : "er" (i) : "memory");
}

static __always_inline void atomic64_sub(long i, atomic64_t *v) {
    asm volatile("lock; subq %1,%0"
                 : "+m" (v->counter)
                 : "er" (i) : "memory");
}

static __always_inline long atomic64_add_return(long i, atomic64_t *v) {
    long __i = i;
    asm volatile("lock; xaddq %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i + __i;
}

static __always_inline long atomic64_sub_return(long i, atomic64_t *v) {
    return atomic64_add_return(-i, v);
}

#define atomic64_inc_return(v)  (atomic64_add_return(1, v))
#define atomic64_dec_return(v)  (atomic64_sub_return(1, v))

static __always_inline void atomic64_inc(atomic64_t *v) {
    asm volatile("lock; incq %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline void atomic64_dec(atomic64_t *v) {
    asm volatile("lock; decq %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline long atomic64_xchg(atomic64_t *v, long new) {
    long ret = new;
    asm volatile("xchgq %0, %1"
                 : "+r" (ret), "+m" (v->counter)
                 : : "memory");
    return ret;
}

static __always_inline long atomic64_cmpxchg(atomic64_t *v, long old, long new) {
    long ret;
    asm volatile("lock; cmpxchgq %2, %1"
                 : "=a" (ret), "+m" (v->counter)
                 : "r" (new), "0" (old)
                 : "memory");
    return ret;
}
