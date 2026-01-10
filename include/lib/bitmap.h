#pragma once

#include <kernel/types.h>
#include <kernel/bitops.h>
#include <lib/string.h>

/**
 * @file include/lib/bitmap.h
 * @brief Advanced Bitmap Management
 * 
 * Optimized for x86_64 using built-ins and bitops.
 */

#define BITS_PER_LONG 64
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

#define DECLARE_BITMAP(name, bits) \
    unsigned long name[((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG]

/*
 * Bit search operations
 */
unsigned long bitmap_find_next_bit(const unsigned long *addr, unsigned long nbits, unsigned long start);
unsigned long bitmap_find_next_zero_bit(const unsigned long *addr, unsigned long nbits, unsigned long start);

static inline unsigned long bitmap_find_first_bit(const unsigned long *addr, unsigned long nbits) {
    return bitmap_find_next_bit(addr, nbits, 0);
}

static inline unsigned long bitmap_find_first_zero_bit(const unsigned long *addr, unsigned long nbits) {
    return bitmap_find_next_zero_bit(addr, nbits, 0);
}

#define find_next_bit bitmap_find_next_bit
#define find_next_zero_bit bitmap_find_next_zero_bit
#define find_first_bit bitmap_find_first_bit
#define find_first_zero_bit bitmap_find_first_zero_bit

/*
 * Find contiguous area of zero bits
 */
unsigned long bitmap_find_next_zero_area(unsigned long *map,
                                         unsigned long size,
                                         unsigned long start,
                                         unsigned int nr,
                                         unsigned long align_mask);

/*
 * Bulk operations
 */
void bitmap_set(unsigned long *map, unsigned int start, int len);
void bitmap_clear(unsigned long *map, unsigned int start, int len);

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits) {
    size_t len = (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG;
    memset(dst, 0, len * sizeof(unsigned long));
}

static inline void bitmap_fill(unsigned long *dst, unsigned int nbits) {
    size_t len = (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG;
    memset(dst, 0xff, len * sizeof(unsigned long));
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits) {
    size_t len = (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG;
    memcpy(dst, src, len * sizeof(unsigned long));
}

/*
 * Testing
 */
bool bitmap_full(const unsigned long *src, unsigned int nbits);
bool bitmap_empty(const unsigned long *src, unsigned int nbits);
bool bitmap_intersects(const unsigned long *src1, const unsigned long *src2, unsigned int nbits);
bool bitmap_subset(const unsigned long *src1, const unsigned long *src2, unsigned int nbits);
unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits);
