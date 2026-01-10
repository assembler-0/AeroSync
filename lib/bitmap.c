/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/bitmap.c
 * @brief High-performance Bitmap management
 * @copyright (C) 2025 assembler-0
 */

#include <lib/bitmap.h>
#include <kernel/bitops.h>

unsigned long bitmap_find_next_bit(const unsigned long *addr, unsigned long nbits, unsigned long start) {
    if (start >= nbits)
        return nbits;

    unsigned long tmp;
    unsigned long word = start / BITS_PER_LONG;
    unsigned long offset = start % BITS_PER_LONG;

    // Handle partial first word
    tmp = addr[word] & (~0UL << offset);
    if (tmp)
        goto found;

    // Search whole words
    while (++word < (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG) {
        tmp = addr[word];
        if (tmp)
            goto found;
    }

    return nbits;

found:
    return word * BITS_PER_LONG + __builtin_ctzl(tmp);
}

unsigned long bitmap_find_next_zero_bit(const unsigned long *addr, unsigned long nbits, unsigned long start) {
    if (start >= nbits)
        return nbits;

    unsigned long tmp;
    unsigned long word = start / BITS_PER_LONG;
    unsigned long offset = start % BITS_PER_LONG;

    // Handle partial first word (invert bits to find zero)
    tmp = ~addr[word] & (~0UL << offset);
    if (tmp)
        goto found;

    // Search whole words
    while (++word < (nbits + BITS_PER_LONG - 1) / BITS_PER_LONG) {
        tmp = ~addr[word];
        if (tmp)
            goto found;
    }

    return nbits;

found:
    return word * BITS_PER_LONG + __builtin_ctzl(tmp);
}

unsigned long bitmap_find_next_zero_area(unsigned long *map,
                                         unsigned long size,
                                         unsigned long start,
                                         unsigned int nr,
                                         unsigned long align_mask) {
    unsigned long index, end, i;
again:
    index = bitmap_find_next_zero_bit(map, size, start);

    /* Align the allocation */
    index = (index + align_mask) & ~align_mask;

    end = index + nr;
    if (end > size)
        return size;

    for (i = index; i < end; i++) {
        if (test_bit(i, map)) {
            start = i + 1;
            goto again;
        }
    }
    return index;
}

void bitmap_set(unsigned long *map, unsigned int start, int len) {
    unsigned long *p = map + BIT_WORD(start);
    const unsigned int size = start + len;
    int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
    unsigned long mask_to_set = ~0UL << (start % BITS_PER_LONG);

    while (len - bits_to_set >= 0) {
        *p |= mask_to_set;
        len -= bits_to_set;
        bits_to_set = BITS_PER_LONG;
        mask_to_set = ~0UL;
        p++;
    }
    if (len) {
        mask_to_set &= ~(~0UL << (size % BITS_PER_LONG));
        *p |= mask_to_set;
    }
}

void bitmap_clear(unsigned long *map, unsigned int start, int len) {
    unsigned long *p = map + BIT_WORD(start);
    const unsigned int size = start + len;
    int bits_to_clear = BITS_PER_LONG - (start % BITS_PER_LONG);
    unsigned long mask_to_clear = ~0UL << (start % BITS_PER_LONG);

    while (len - bits_to_clear >= 0) {
        *p &= ~mask_to_clear;
        len -= bits_to_clear;
        bits_to_clear = BITS_PER_LONG;
        mask_to_clear = ~0UL;
        p++;
    }
    if (len) {
        mask_to_clear &= ~(~0UL << (size % BITS_PER_LONG));
        *p &= ~mask_to_clear;
    }
}

bool bitmap_full(const unsigned long *src, unsigned int nbits) {
    unsigned int words = nbits / BITS_PER_LONG;
    unsigned int left = nbits % BITS_PER_LONG;

    for (unsigned int i = 0; i < words; i++) {
        if (src[i] != ~0UL) return false;
    }

    if (left) {
        unsigned long mask = (1UL << left) - 1;
        if ((src[words] & mask) != mask) return false;
    }

    return true;
}

bool bitmap_empty(const unsigned long *src, unsigned int nbits) {
    unsigned int words = nbits / BITS_PER_LONG;
    unsigned int left = nbits % BITS_PER_LONG;

    for (unsigned int i = 0; i < words; i++) {
        if (src[i] != 0) return false;
    }

    if (left) {
        unsigned long mask = (1UL << left) - 1;
        if ((src[words] & mask) != 0) return false;
    }

    return true;
}

unsigned int bitmap_weight(const unsigned long *src, unsigned int nbits) {
    unsigned int words = nbits / BITS_PER_LONG;
    unsigned int left = nbits % BITS_PER_LONG;
    unsigned int weight = 0;

    for (unsigned int i = 0; i < words; i++) {
        weight += __builtin_popcountl(src[i]);
    }

    if (left) {
        unsigned long mask = (1UL << left) - 1;
        weight += __builtin_popcountl(src[words] & mask);
    }

    return weight;
}