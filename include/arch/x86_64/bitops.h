#pragma once

#include <aerosync/types.h>
#include <compiler.h>

/**
 * @file include/arch/x86_64/bitops.h
 * @brief Optimized x86_64 bit operations (Linux-style)
 */

static __always_inline void set_bit(long nr, volatile unsigned long *addr) {
    __asm__ volatile("lock; bts %1,%0"
                 : "+m" (*addr) : "Ir" (nr) : "memory");
}

static __always_inline void clear_bit(long nr, volatile unsigned long *addr) {
    __asm__ volatile("lock; btr %1,%0"
                 : "+m" (*addr) : "Ir" (nr) : "memory");
}

static __always_inline void change_bit(long nr, volatile unsigned long *addr) {
    __asm__ volatile("lock; btc %1,%0"
                 : "+m" (*addr) : "Ir" (nr) : "memory");
}

static __always_inline bool test_and_set_bit(long nr, volatile unsigned long *addr) {
    bool oldbit;
    __asm__ volatile("lock; bts %2,%1; setc %0"
                 : "=qm" (oldbit), "+m" (*addr)
                 : "Ir" (nr) : "memory");
    return oldbit;
}

static __always_inline bool test_and_clear_bit(long nr, volatile unsigned long *addr) {
    bool oldbit;
    __asm__ volatile("lock; btr %2,%1; setc %0"
                 : "=qm" (oldbit), "+m" (*addr)
                 : "Ir" (nr) : "memory");
    return oldbit;
}

static __always_inline bool test_and_change_bit(long nr, volatile unsigned long *addr) {
    bool oldbit;
    __asm__ volatile("lock; btc %2,%1; setc %0"
                 : "=qm" (oldbit), "+m" (*addr)
                 : "Ir" (nr) : "memory");
    return oldbit;
}

static __always_inline bool test_bit(long nr, const volatile unsigned long *addr) {
    return ((1UL << (nr & (64 - 1))) & (addr[nr >> 6])) != 0;
}

/* Non-atomic variants */
static __always_inline void __set_bit(long nr, volatile unsigned long *addr) {
    __asm__ volatile("bts %1,%0" : "+m" (*addr) : "Ir" (nr) : "memory");
}

static __always_inline void __clear_bit(long nr, volatile unsigned long *addr) {
    __asm__ volatile("btr %1,%0" : "+m" (*addr) : "Ir" (nr) : "memory");
}
