#pragma once

#include <kernel/types.h>

/**
 * @file include/kernel/atomic.h
 * @brief Atomic operations using compiler built-ins
 */

static inline int atomic_read(const atomic_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
}

static inline void atomic_set(atomic_t *v, int i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic_add(int i, atomic_t *v) {
    __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_sub(int i, atomic_t *v) {
    __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_add_return(int i, atomic_t *v) {
    return __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline int atomic_sub_return(int i, atomic_t *v) {
    return __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_inc(atomic_t *v) {
    atomic_add(1, v);
}

static inline void atomic_dec(atomic_t *v) {
    atomic_sub(1, v);
}

static inline int atomic_inc_return(atomic_t *v) {
    return atomic_add_return(1, v);
}

static inline int atomic_dec_return(atomic_t *v) {
    return atomic_sub_return(1, v);
}

static inline int atomic_dec_and_test(atomic_t *v) {
    return atomic_dec_return(v) == 0;
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new) {
    int expected = old;
    __atomic_compare_exchange_n(&v->counter, &expected, new, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

static inline int atomic_xchg(atomic_t *v, int new) {
    return __atomic_exchange_n(&v->counter, new, __ATOMIC_SEQ_CST);
}

typedef struct {
    volatile long counter;
} atomic_long_t;

static inline long atomic_long_read(const atomic_long_t *v) {
    return __atomic_load_n(&v->counter, __ATOMIC_RELAXED);
}

static inline void atomic_long_set(atomic_long_t *v, long i) {
    __atomic_store_n(&v->counter, i, __ATOMIC_RELAXED);
}

static inline void atomic_long_add(long i, atomic_long_t *v) {
    __atomic_add_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}

static inline void atomic_long_sub(long i, atomic_long_t *v) {
    __atomic_sub_fetch(&v->counter, i, __ATOMIC_SEQ_CST);
}