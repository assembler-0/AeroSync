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
