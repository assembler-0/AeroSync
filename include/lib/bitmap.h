#pragma once

#include <kernel/types.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define BIT_MASK(nr) (1UL << ((nr) % BITS_PER_LONG))
#define BIT_WORD(nr) ((nr) / BITS_PER_LONG)

// Set bit
static inline void set_bit(int nr, volatile unsigned long *addr) {
    addr[BIT_WORD(nr)] |= BIT_MASK(nr);
}

// Clear bit
static inline void clear_bit(int nr, volatile unsigned long *addr) {
    addr[BIT_WORD(nr)] &= ~BIT_MASK(nr);
}

// Test bit
static inline bool test_bit(int nr, const volatile unsigned long *addr) {
    return (addr[BIT_WORD(nr)] & BIT_MASK(nr)) != 0;
}

// Find first zero bit
int find_first_zero_bit(const unsigned long *addr, unsigned size);

// Find next zero bit
int find_next_zero_bit(const unsigned long *addr, unsigned size, unsigned offset);

// Find first set bit
int find_first_bit(const unsigned long *addr, unsigned size);

// Find next set bit
int find_next_bit(const unsigned long *addr, unsigned size, unsigned offset);