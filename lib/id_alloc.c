/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file lib/id_alloc.c
 * @brief Generic ID Allocator (IDA) implementation
 * @copyright (C) 2025 assembler-0
 */

#include <lib/id_alloc.h>
#include <lib/bitmap.h>
#include <mm/slab.h>
#include <mm/vmalloc.h>
#include <lib/string.h>

void ida_init(struct ida *ida, int max_id) {
    int bitmap_size = (max_id + BITS_PER_LONG - 1) / BITS_PER_LONG;
    ida->bitmap = vzalloc(bitmap_size * sizeof(unsigned long));
    if (!ida->bitmap) {
        // Fallback or panic if critical
        return;
    }
    ida->max_id = max_id;
    ida->last_id = 0;
    ida->lock = 0;
}

int ida_alloc_min(struct ida *ida, int min) {
    spinlock_lock(&ida->lock);
    
    int id = find_next_zero_bit(ida->bitmap, ida->max_id, min);
    if (id >= ida->max_id) {
        // Wrap around and search from 0 if min was not 0
        if (min > 0) {
            id = find_next_zero_bit(ida->bitmap, min, 0);
        }
        
        if (id >= min && min > 0) {
            spinlock_unlock(&ida->lock);
            return -1;
        }
        
        if (id >= ida->max_id) {
            spinlock_unlock(&ida->lock);
            return -1;
        }
    }
    
    set_bit(id, ida->bitmap);
    ida->last_id = id;
    
    spinlock_unlock(&ida->lock);
    return id;
}

int ida_alloc(struct ida *ida) {
    return ida_alloc_min(ida, 0);
}

void ida_free(struct ida *ida, int id) {
    if (id < 0 || id >= ida->max_id)
        return;
    
    spinlock_lock(&ida->lock);
    clear_bit(id, ida->bitmap);
    spinlock_unlock(&ida->lock);
}
