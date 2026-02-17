#pragma once

#include <aerosync/types.h>

/**
 * @struct ringbuf
 * @brief Simple circular buffer implementation
 * @deprecated Use kfifo (include/linux/kfifo.h) for new code.
 *             This subsystem is scheduled for removal.
 */
struct ringbuf {
    uint8_t *data;
    size_t size;
    size_t head;
    size_t tail;
};

typedef struct ringbuf ringbuf_t;

// Create a new ring buffer on the heap
// @deprecated Use kfifo_alloc instead
struct ringbuf *ringbuf_create(size_t size);

// Destroy a heap-allocated ring buffer
// @deprecated Use kfifo_free instead
void ringbuf_destroy(struct ringbuf *rb);

// Initialize ring buffer
// @deprecated Use kfifo_init instead
void ringbuf_init(struct ringbuf *rb, void *buffer, size_t size);

// Get available space
// @deprecated Use kfifo_avail instead
size_t ringbuf_space(const struct ringbuf *rb);

// Get used space
// @deprecated Use kfifo_len instead
size_t ringbuf_used(const struct ringbuf *rb);

// Check if empty/full
// @deprecated Use kfifo_is_empty instead
bool ringbuf_empty(const struct ringbuf *rb);
// @deprecated Use kfifo_is_full instead
bool ringbuf_full(const struct ringbuf *rb);

// Write data (returns bytes written)
// @deprecated Use kfifo_in instead
size_t ringbuf_write(struct ringbuf *rb, const void *data, size_t len);

// Read data (returns bytes read)
// @deprecated Use kfifo_out instead
size_t ringbuf_read(struct ringbuf *rb, void *data, size_t len);

// Peek data without consuming
// @deprecated Use kfifo_out_peek instead
size_t ringbuf_peek(const struct ringbuf *rb, void *data, size_t len);

// Skip/consume data without reading
// @deprecated Use kfifo_skip_count instead
void ringbuf_skip(struct ringbuf *rb, size_t len);

// Reset buffer
// @deprecated Use kfifo_reset instead
void ringbuf_reset(struct ringbuf *rb);