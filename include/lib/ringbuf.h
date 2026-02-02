#pragma once

#include <aerosync/types.h>

struct ringbuf {
    uint8_t *data;
    size_t size;
    size_t head;
    size_t tail;
};

typedef struct ringbuf ringbuf_t;

// Create a new ring buffer on the heap
struct ringbuf *ringbuf_create(size_t size);

// Destroy a heap-allocated ring buffer
void ringbuf_destroy(struct ringbuf *rb);

// Initialize ring buffer
void ringbuf_init(struct ringbuf *rb, void *buffer, size_t size);

// Get available space
size_t ringbuf_space(const struct ringbuf *rb);

// Get used space
size_t ringbuf_used(const struct ringbuf *rb);

// Check if empty/full
bool ringbuf_empty(const struct ringbuf *rb);
bool ringbuf_full(const struct ringbuf *rb);

// Write data (returns bytes written)
size_t ringbuf_write(struct ringbuf *rb, const void *data, size_t len);

// Read data (returns bytes read)
size_t ringbuf_read(struct ringbuf *rb, void *data, size_t len);

// Peek data without consuming
size_t ringbuf_peek(const struct ringbuf *rb, void *data, size_t len);

// Skip/consume data without reading
void ringbuf_skip(struct ringbuf *rb, size_t len);

// Reset buffer
void ringbuf_reset(struct ringbuf *rb);

// Destroy buffer
void ringbuf_destroy(ringbuf_t *rb);