#pragma once

#include <kernel/types.h>

typedef struct {
    uint8_t *data;
    size_t size;
    size_t head;
    size_t tail;
} ringbuf_t;

// Initialize ring buffer
void ringbuf_init(ringbuf_t *rb, void *buffer, size_t size);

// Get available space
size_t ringbuf_space(const ringbuf_t *rb);

// Get used space
size_t ringbuf_used(const ringbuf_t *rb);

// Check if empty/full
bool ringbuf_empty(const ringbuf_t *rb);
bool ringbuf_full(const ringbuf_t *rb);

// Write data (returns bytes written)
size_t ringbuf_write(ringbuf_t *rb, const void *data, size_t len);

// Read data (returns bytes read)
size_t ringbuf_read(ringbuf_t *rb, void *data, size_t len);

// Peek data without consuming
size_t ringbuf_peek(const ringbuf_t *rb, void *data, size_t len);

// Skip/consume data without reading
void ringbuf_skip(ringbuf_t *rb, size_t len);

// Reset buffer
void ringbuf_reset(ringbuf_t *rb);