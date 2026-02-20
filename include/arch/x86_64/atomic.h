#pragma once

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
 * Internal helpers for generic macros
 */

#define __cmpxchg(ptr, old, new, size)                                  \
({                                                                      \
    __typeof__(*(ptr)) __ret;                                           \
    __asm__ volatile("lock; cmpxchg" size " %2, %1"                     \
                     : "=a" (__ret), "+m" (*(ptr))                      \
                     : "r" ((__typeof__(*(ptr)))(new)),                 \
                       "0" ((__typeof__(*(ptr)))(old))                  \
                     : "memory");                                       \
    __ret;                                                              \
})

#define __xchg(ptr, new, size)                                          \
({                                                                      \
    __typeof__(*(ptr)) __ret = (__typeof__(*(ptr)))(new);               \
    __asm__ volatile("xchg" size " %0, %1"                              \
                     : "+r" (__ret), "+m" (*(ptr))                      \
                     : : "memory");                                     \
    __ret;                                                              \
})

/*
 * Generic xchg and cmpxchg
 */

#define xchg(ptr, new)                                                  \
({                                                                      \
    __typeof__(*(ptr)) __res;                                           \
    switch (sizeof(*(ptr))) {                                           \
    case 8: __res = __xchg(ptr, new, "q"); break;                       \
    case 4: __res = __xchg(ptr, new, "l"); break;                       \
    case 2: __res = __xchg(ptr, new, "w"); break;                       \
    case 1: __res = __xchg(ptr, new, "b"); break;                       \
    default: __builtin_trap();                                          \
    }                                                                   \
    __res;                                                              \
})

#define cmpxchg(ptr, old, new)                                          \
({                                                                      \
    __typeof__(*(ptr)) __res;                                           \
    switch (sizeof(*(ptr))) {                                           \
    case 8: __res = __cmpxchg(ptr, old, new, "q"); break;               \
    case 4: __res = __cmpxchg(ptr, old, new, "l"); break;               \
    case 2: __res = __cmpxchg(ptr, old, new, "w"); break;               \
    case 1: __res = __cmpxchg(ptr, old, new, "b"); break;               \
    default: __builtin_trap();                                          \
    }                                                                   \
    __res;                                                              \
})

#define __try_cmpxchg(ptr, pold, new, size)                             \
({                                                                      \
    bool __success;                                                     \
    __typeof__(*(ptr)) __old = *(pold);                                 \
    __asm__ volatile("lock; cmpxchg" size " %2, %1"                     \
                     : "=@ccz" (__success), "+m" (*(ptr)), "+a" (__old) \
                     : "r" ((__typeof__(*(ptr)))(new))                  \
                     : "memory");                                       \
    if (unlikely(!__success))                                           \
        *(pold) = __old;                                                \
    __success;                                                          \
})

#define try_cmpxchg(ptr, pold, new)                                     \
({                                                                      \
    bool __res;                                                         \
    switch (sizeof(*(ptr))) {                                           \
    case 8: __res = __try_cmpxchg(ptr, pold, new, "q"); break;          \
    case 4: __res = __try_cmpxchg(ptr, pold, new, "l"); break;          \
    case 2: __res = __try_cmpxchg(ptr, pold, new, "w"); break;          \
    case 1: __res = __try_cmpxchg(ptr, pold, new, "b"); break;          \
    default: __builtin_trap();                                          \
    }                                                                   \
    __res;                                                              \
})

#define try_cmpxchg64(ptr, pold, new) __try_cmpxchg(ptr, pold, new, "q")

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
    __asm__ volatile("lock; addl %1,%0"
                 : "+m" (v->counter)
                 : "ir" (i) : "memory");
}

static __always_inline void atomic_sub(int i, atomic_t *v) {
    __asm__ volatile("lock; subl %1,%0"
                 : "+m" (v->counter)
                 : "ir" (i) : "memory");
}

static __always_inline int atomic_add_return(int i, atomic_t *v) {
    int __i = i;
    __asm__ volatile("lock; xaddl %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i + __i;
}

static __always_inline int atomic_sub_return(int i, atomic_t *v) {
    return atomic_add_return(-i, v);
}

static __always_inline int atomic_fetch_add(int i, atomic_t *v) {
    __asm__ volatile("lock; xaddl %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i;
}

static __always_inline int atomic_fetch_sub(int i, atomic_t *v) {
    return atomic_fetch_add(-i, v);
}

static __always_inline void atomic_and(int i, atomic_t *v) {
    __asm__ volatile("lock; andl %1,%0" : "+m" (v->counter) : "ir" (i) : "memory");
}

static __always_inline void atomic_or(int i, atomic_t *v) {
    __asm__ volatile("lock; orl %1,%0" : "+m" (v->counter) : "ir" (i) : "memory");
}

static __always_inline void atomic_xor(int i, atomic_t *v) {
    __asm__ volatile("lock; xorl %1,%0" : "+m" (v->counter) : "ir" (i) : "memory");
}

#define atomic_inc_return(v)  (atomic_add_return(1, v))
#define atomic_dec_return(v)  (atomic_sub_return(1, v))

static __always_inline bool atomic_sub_and_test(int i, atomic_t *v) {
    bool c;
    __asm__ volatile("lock; subl %2,%0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : "ir" (i) : "memory");
    return c;
}

static __always_inline void atomic_inc(atomic_t *v) {
    __asm__ volatile("lock; incl %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline void atomic_dec(atomic_t *v) {
    __asm__ volatile("lock; decl %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline bool atomic_dec_and_test(atomic_t *v) {
    bool c;
    __asm__ volatile("lock; decl %0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : : "memory");
    return c;
}

static __always_inline bool atomic_inc_and_test(atomic_t *v) {
    bool c;
    __asm__ volatile("lock; incl %0; setz %1"
                 : "+m" (v->counter), "=qn" (c)
                 : : "memory");
    return c;
}

static __always_inline int atomic_xchg(atomic_t *v, int new) {
    return xchg(&v->counter, new);
}

static __always_inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    return cmpxchg(&v->counter, old, new);
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
    __asm__ volatile("lock; addq %1,%0"
                 : "+m" (v->counter)
                 : "er" (i) : "memory");
}

static __always_inline void atomic64_sub(long i, atomic64_t *v) {
    __asm__ volatile("lock; subq %1,%0"
                 : "+m" (v->counter)
                 : "er" (i) : "memory");
}

static __always_inline long atomic64_add_return(long i, atomic64_t *v) {
    long __i = i;
    __asm__ volatile("lock; xaddq %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i + __i;
}

static __always_inline long atomic64_sub_return(long i, atomic64_t *v) {
    return atomic64_add_return(-i, v);
}

static __always_inline long atomic64_fetch_add(long i, atomic64_t *v) {
    __asm__ volatile("lock; xaddq %0, %1"
                 : "+r" (i), "+m" (v->counter)
                 : : "memory");
    return i;
}

static __always_inline long atomic64_fetch_sub(long i, atomic64_t *v) {
    return atomic64_fetch_add(-i, v);
}

static __always_inline void atomic64_and(long i, atomic64_t *v) {
    __asm__ volatile("lock; andq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}

static __always_inline void atomic64_or(long i, atomic64_t *v) {
    __asm__ volatile("lock; orq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}

static __always_inline void atomic64_xor(long i, atomic64_t *v) {
    __asm__ volatile("lock; xorq %1,%0" : "+m" (v->counter) : "er" (i) : "memory");
}

#define atomic64_inc_return(v)  (atomic64_add_return(1, v))
#define atomic64_dec_return(v)  (atomic64_sub_return(1, v))

static __always_inline void atomic64_inc(atomic64_t *v) {
    __asm__ volatile("lock; incq %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline void atomic64_dec(atomic64_t *v) {
    __asm__ volatile("lock; decq %0"
                 : "+m" (v->counter) : : "memory");
}

static __always_inline long atomic64_xchg(atomic64_t *v, long new) {
    return xchg(&v->counter, new);
}

static __always_inline long atomic64_cmpxchg(atomic64_t *v, long old, long new) {
    return cmpxchg(&v->counter, old, new);
}

/*
 * Double-width cmpxchg on absolute address.
 * Targets two adjacent 64-bit values (16 bytes total).
 * Must be 16-byte aligned.
 */
static __always_inline bool cmpxchg16b_local(void *ptr, void *o1, unsigned long o2,
                                    void *n1, unsigned long n2) {
  bool ret;
  __asm__ volatile("lock; cmpxchg16b %1; setz %0"
               : "=q"(ret), "+m"(*(char *)ptr), "+d"(o2), "+a"(o1)
               : "b"(n1), "c"(n2)
               : "memory");
  return ret;
}


/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Generic barrier definitions.
 *
 * It should be possible to use these on really simple architectures,
 * but it serves more as a starting point for new ports.
 *
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */
#ifndef __ASM_GENERIC_BARRIER_H
#define __ASM_GENERIC_BARRIER_H

#ifndef __ASSEMBLY__

#ifndef nop
#define nop()	asm volatile ("nop")
#endif

/*
 * Architectures that want generic instrumentation can define __ prefixed
 * variants of all barriers.
 */

#ifdef __mb
#define mb()	do { kcsan_mb(); __mb(); } while (0)
#endif

#ifdef __rmb
#define rmb()	do { kcsan_rmb(); __rmb(); } while (0)
#endif

#ifdef __wmb
#define wmb()	do { kcsan_wmb(); __wmb(); } while (0)
#endif

#ifdef __dma_mb
#define dma_mb()	do { kcsan_mb(); __dma_mb(); } while (0)
#endif

#ifdef __dma_rmb
#define dma_rmb()	do { kcsan_rmb(); __dma_rmb(); } while (0)
#endif

#ifdef __dma_wmb
#define dma_wmb()	do { kcsan_wmb(); __dma_wmb(); } while (0)
#endif

/*
 * Force strict CPU ordering. And yes, this is required on UP too when we're
 * talking to devices.
 *
 * Fall back to compiler barriers if nothing better is provided.
 */

#ifndef mb
#define mb()	cbarrier()
#endif

#ifndef rmb
#define rmb()	mb()
#endif

#ifndef wmb
#define wmb()	mb()
#endif

#ifndef dma_mb
#define dma_mb()	mb()
#endif

#ifndef dma_rmb
#define dma_rmb()	rmb()
#endif

#ifndef dma_wmb
#define dma_wmb()	wmb()
#endif

#ifndef __smp_mb
#define __smp_mb()	mb()
#endif

#ifndef __smp_rmb
#define __smp_rmb()	rmb()
#endif

#ifndef __smp_wmb
#define __smp_wmb()	wmb()
#endif

#ifdef CONFIG_SMP

#ifndef smp_mb
#define smp_mb()	do { kcsan_mb(); __smp_mb(); } while (0)
#endif

#ifndef smp_rmb
#define smp_rmb()	do { kcsan_rmb(); __smp_rmb(); } while (0)
#endif

#ifndef smp_wmb
#define smp_wmb()	do { kcsan_wmb(); __smp_wmb(); } while (0)
#endif

#else	/* !CONFIG_SMP */

#ifndef smp_mb
#define smp_mb()	barrier()
#endif

#ifndef smp_rmb
#define smp_rmb()	barrier()
#endif

#ifndef smp_wmb
#define smp_wmb()	barrier()
#endif

#endif	/* CONFIG_SMP */

#ifndef __smp_store_mb
#define __smp_store_mb(var, value)  do { WRITE_ONCE(var, value); __smp_mb(); } while (0)
#endif

#ifndef __smp_mb__before_atomic
#define __smp_mb__before_atomic()	__smp_mb()
#endif

#ifndef __smp_mb__after_atomic
#define __smp_mb__after_atomic()	__smp_mb()
#endif

#ifndef __smp_store_release
#define __smp_store_release(p, v)					\
do {									\
	compiletime_assert_atomic_type(*p);				\
	__smp_mb();							\
	WRITE_ONCE(*p, v);						\
} while (0)
#endif

#ifndef __smp_load_acquire
#define __smp_load_acquire(p)						\
({									\
	__unqual_scalar_typeof(*p) ___p1 = READ_ONCE(*p);		\
	compiletime_assert_atomic_type(*p);				\
	__smp_mb();							\
	(typeof(*p))___p1;						\
})
#endif

#ifdef CONFIG_SMP

#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { kcsan_mb(); __smp_store_mb(var, value); } while (0)
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic()	do { kcsan_mb(); __smp_mb__before_atomic(); } while (0)
#endif

#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic()	do { kcsan_mb(); __smp_mb__after_atomic(); } while (0)
#endif

#ifndef smp_store_release
#define smp_store_release(p, v) do { kcsan_release(); __smp_store_release(p, v); } while (0)
#endif

#ifndef smp_load_acquire
#define smp_load_acquire(p) __smp_load_acquire(p)
#endif

#else	/* !CONFIG_SMP */

#ifndef smp_store_mb
#define smp_store_mb(var, value)  do { WRITE_ONCE(var, value); barrier(); } while (0)
#endif

#ifndef smp_mb__before_atomic
#define smp_mb__before_atomic()	barrier()
#endif

#ifndef smp_mb__after_atomic
#define smp_mb__after_atomic()	barrier()
#endif

#ifndef smp_store_release
#define smp_store_release(p, v)						\
do {									\
	barrier();							\
	WRITE_ONCE(*p, v);						\
} while (0)
#endif

#ifndef smp_load_acquire
#define smp_load_acquire(p)						\
({									\
	__unqual_scalar_typeof(*p) ___p1 = READ_ONCE(*p);		\
	barrier();							\
	(typeof(*p))___p1;						\
})
#endif

#endif	/* CONFIG_SMP */

/* Barriers for virtual machine guests when talking to an SMP host */
#define virt_mb() do { kcsan_mb(); __smp_mb(); } while (0)
#define virt_rmb() do { kcsan_rmb(); __smp_rmb(); } while (0)
#define virt_wmb() do { kcsan_wmb(); __smp_wmb(); } while (0)
#define virt_store_mb(var, value) do { kcsan_mb(); __smp_store_mb(var, value); } while (0)
#define virt_mb__before_atomic() do { kcsan_mb(); __smp_mb__before_atomic(); } while (0)
#define virt_mb__after_atomic()	do { kcsan_mb(); __smp_mb__after_atomic(); } while (0)
#define virt_store_release(p, v) do { kcsan_release(); __smp_store_release(p, v); } while (0)
#define virt_load_acquire(p) __smp_load_acquire(p)

/**
 * smp_acquire__after_ctrl_dep() - Provide ACQUIRE ordering after a control dependency
 *
 * A control dependency provides a LOAD->STORE order, the additional RMB
 * provides LOAD->LOAD order, together they provide LOAD->{LOAD,STORE} order,
 * aka. (load)-ACQUIRE.
 *
 * Architectures that do not do load speculation can have this be barrier().
 */
#ifndef smp_acquire__after_ctrl_dep
#define smp_acquire__after_ctrl_dep()		smp_rmb()
#endif

/**
 * smp_cond_load_relaxed() - (Spin) wait for cond with no ordering guarantees
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using READ_ONCE() on the condition variable.
 *
 * Due to C lacking lambda expressions we load the value of *ptr into a
 * pre-named variable @VAL to be used in @cond.
 */
#ifndef smp_cond_load_relaxed
#define smp_cond_load_relaxed(ptr, cond_expr) ({		\
	typeof(ptr) __PTR = (ptr);				\
	__unqual_scalar_typeof(*ptr) VAL;			\
	for (;;) {						\
		VAL = READ_ONCE(*__PTR);			\
		if (cond_expr)					\
			break;					\
		cpu_relax();					\
	}							\
	(typeof(*ptr))VAL;					\
})
#endif

/**
 * smp_cond_load_acquire() - (Spin) wait for cond with ACQUIRE ordering
 * @ptr: pointer to the variable to wait on
 * @cond: boolean expression to wait for
 *
 * Equivalent to using smp_load_acquire() on the condition variable but employs
 * the control dependency of the wait to reduce the barrier on many platforms.
 */
#ifndef smp_cond_load_acquire
#define smp_cond_load_acquire(ptr, cond_expr) ({		\
	__unqual_scalar_typeof(*ptr) _val;			\
	_val = smp_cond_load_relaxed(ptr, cond_expr);		\
	smp_acquire__after_ctrl_dep();				\
	(typeof(*ptr))_val;					\
})
#endif

/*
 * pmem_wmb() ensures that all stores for which the modification
 * are written to persistent storage by preceding instructions have
 * updated persistent storage before any data  access or data transfer
 * caused by subsequent instructions is initiated.
 */
#ifndef pmem_wmb
#define pmem_wmb()	wmb()
#endif

/*
 * ioremap_wc() maps I/O memory as memory with write-combining attributes. For
 * this kind of memory accesses, the CPU may wait for prior accesses to be
 * merged with subsequent ones. In some situation, such wait is bad for the
 * performance. io_stop_wc() can be used to prevent the merging of
 * write-combining memory accesses before this macro with those after it.
 */
#ifndef io_stop_wc
#define io_stop_wc() do { } while (0)
#endif

/*
 * Architectures that guarantee an implicit smp_mb() in switch_mm()
 * can override smp_mb__after_switch_mm.
 */
#ifndef smp_mb__after_switch_mm
# define smp_mb__after_switch_mm()	smp_mb()
#endif

#endif /* !__ASSEMBLY__ */
#endif /* __ASM_GENERIC_BARRIER_H */