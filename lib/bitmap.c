#include <lib/bitmap.h>

int find_first_zero_bit(const unsigned long *addr, unsigned size) {
    unsigned long idx, tmp;
    
    for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
        if (addr[idx] != ~0UL) {
            tmp = addr[idx];
            for (int bit = 0; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
                if (!(tmp & (1UL << bit))) {
                    return idx * BITS_PER_LONG + bit;
                }
            }
        }
    }
    return size;
}

int find_next_zero_bit(const unsigned long *addr, unsigned size, unsigned offset) {
    if (offset >= size)
        return size;
    
    unsigned long idx = offset / BITS_PER_LONG;
    unsigned long tmp = addr[idx];
    
    // Mask off bits before offset
    tmp |= (1UL << (offset % BITS_PER_LONG)) - 1;
    
    if (tmp != ~0UL) {
        for (int bit = offset % BITS_PER_LONG; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
            if (!(tmp & (1UL << bit))) {
                return idx * BITS_PER_LONG + bit;
            }
        }
    }
    
    // Search remaining words
    for (idx++; idx * BITS_PER_LONG < size; idx++) {
        if (addr[idx] != ~0UL) {
            tmp = addr[idx];
            for (int bit = 0; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
                if (!(tmp & (1UL << bit))) {
                    return idx * BITS_PER_LONG + bit;
                }
            }
        }
    }
    return size;
}

int find_first_bit(const unsigned long *addr, unsigned size) {
    unsigned long idx, tmp;
    
    for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
        if (addr[idx] != 0) {
            tmp = addr[idx];
            for (int bit = 0; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
                if (tmp & (1UL << bit)) {
                    return idx * BITS_PER_LONG + bit;
                }
            }
        }
    }
    return size;
}

int find_next_bit(const unsigned long *addr, unsigned size, unsigned offset) {
    if (offset >= size)
        return size;
    
    unsigned long idx = offset / BITS_PER_LONG;
    unsigned long tmp = addr[idx];
    
    // Mask off bits before offset
    tmp &= ~((1UL << (offset % BITS_PER_LONG)) - 1);
    
    if (tmp != 0) {
        for (int bit = offset % BITS_PER_LONG; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
            if (tmp & (1UL << bit)) {
                return idx * BITS_PER_LONG + bit;
            }
        }
    }
    
    // Search remaining words
    for (idx++; idx * BITS_PER_LONG < size; idx++) {
        if (addr[idx] != 0) {
            tmp = addr[idx];
            for (int bit = 0; bit < BITS_PER_LONG && idx * BITS_PER_LONG + bit < size; bit++) {
                if (tmp & (1UL << bit)) {
                    return idx * BITS_PER_LONG + bit;
                }
            }
        }
    }
    return size;
}