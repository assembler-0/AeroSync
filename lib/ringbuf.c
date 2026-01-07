/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/ringbuf.c
 * @brief Circular buffer implementation
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <lib/ringbuf.h>
#include <lib/string.h>

void ringbuf_init(ringbuf_t *rb, void *buffer, size_t size) {
    rb->data = (uint8_t *)buffer;
    rb->size = size;
    rb->head = 0;
    rb->tail = 0;
}

size_t ringbuf_space(const ringbuf_t *rb) {
    if (rb->head >= rb->tail)
        return rb->size - (rb->head - rb->tail) - 1;
    return rb->tail - rb->head - 1;
}

size_t ringbuf_used(const ringbuf_t *rb) {
    if (rb->head >= rb->tail)
        return rb->head - rb->tail;
    return rb->size - (rb->tail - rb->head);
}

bool ringbuf_empty(const ringbuf_t *rb) {
    return rb->head == rb->tail;
}

bool ringbuf_full(const ringbuf_t *rb) {
    return ringbuf_space(rb) == 0;
}

size_t ringbuf_write(ringbuf_t *rb, const void *data, size_t len) {
    const uint8_t *src = (const uint8_t *)data;
    size_t space = ringbuf_space(rb);
    
    if (len > space)
        len = space;
    
    if (len == 0)
        return 0;
    
    size_t to_end = rb->size - rb->head;
    if (len <= to_end) {
        memcpy(&rb->data[rb->head], src, len);
        rb->head = (rb->head + len) % rb->size;
    } else {
        memcpy(&rb->data[rb->head], src, to_end);
        memcpy(&rb->data[0], src + to_end, len - to_end);
        rb->head = len - to_end;
    }
    
    return len;
}

size_t ringbuf_read(ringbuf_t *rb, void *data, size_t len) {
    uint8_t *dst = (uint8_t *)data;
    size_t used = ringbuf_used(rb);
    
    if (len > used)
        len = used;
    
    if (len == 0)
        return 0;
    
    size_t to_end = rb->size - rb->tail;
    if (len <= to_end) {
        memcpy(dst, &rb->data[rb->tail], len);
        rb->tail = (rb->tail + len) % rb->size;
    } else {
        memcpy(dst, &rb->data[rb->tail], to_end);
        memcpy(dst + to_end, &rb->data[0], len - to_end);
        rb->tail = len - to_end;
    }
    
    return len;
}

size_t ringbuf_peek(const ringbuf_t *rb, void *data, size_t len) {
    uint8_t *dst = (uint8_t *)data;
    size_t used = ringbuf_used(rb);
    
    if (len > used)
        len = used;
    
    if (len == 0)
        return 0;
    
    size_t to_end = rb->size - rb->tail;
    if (len <= to_end) {
        memcpy(dst, &rb->data[rb->tail], len);
    } else {
        memcpy(dst, &rb->data[rb->tail], to_end);
        memcpy(dst + to_end, &rb->data[0], len - to_end);
    }
    
    return len;
}

void ringbuf_skip(ringbuf_t *rb, size_t len) {
    size_t used = ringbuf_used(rb);
    if (len > used)
        len = used;
    rb->tail = (rb->tail + len) % rb->size;
}

void ringbuf_reset(ringbuf_t *rb) {
    rb->head = 0;
    rb->tail = 0;
}