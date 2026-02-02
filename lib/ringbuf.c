/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file lib/ringbuf.c
 * @brief Circular buffer implementation
 * @copyright (C) 2025-2026 assembler-0
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
#include <aerosync/export.h>
#include <mm/slub.h>

struct ringbuf *ringbuf_create(size_t size) {
  struct ringbuf *rb = kzalloc(sizeof(struct ringbuf));
  if (!rb) return nullptr;

  void *buffer = kmalloc(size);
  if (!buffer) {
    kfree(rb);
    return nullptr;
  }

  ringbuf_init(rb, buffer, size);
  return rb;
}
EXPORT_SYMBOL(ringbuf_create);

void ringbuf_init(struct ringbuf *rb, void *buffer, size_t size) {
  rb->data = (uint8_t *) buffer;
  rb->size = size;
  ringbuf_reset(rb);
}
EXPORT_SYMBOL(ringbuf_init);

void ringbuf_destroy(struct ringbuf *rb) {
  if (!rb) return;
  if (rb->data) kfree(rb->data);
  kfree(rb);
}
EXPORT_SYMBOL(ringbuf_destroy);

size_t ringbuf_space(const struct ringbuf *rb) {
  if (rb->head >= rb->tail)
    return rb->size - (rb->head - rb->tail) - 1;
  return rb->tail - rb->head - 1;
}
EXPORT_SYMBOL(ringbuf_space);

size_t ringbuf_used(const struct ringbuf *rb) {
  if (rb->head >= rb->tail)
    return rb->head - rb->tail;
  return rb->size - (rb->tail - rb->head);
}
EXPORT_SYMBOL(ringbuf_used);

bool ringbuf_empty(const struct ringbuf *rb) {
  return rb->head == rb->tail;
}
EXPORT_SYMBOL(ringbuf_empty);

bool ringbuf_full(const struct ringbuf *rb) {
  return ringbuf_space(rb) == 0;
}
EXPORT_SYMBOL(ringbuf_full);

size_t ringbuf_write(struct ringbuf *rb, const void *data, size_t len) {
  const uint8_t *src = (const uint8_t *) data;
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
EXPORT_SYMBOL(ringbuf_write);

size_t ringbuf_read(struct ringbuf *rb, void *data, size_t len) {
  uint8_t *dst = (uint8_t *) data;
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
EXPORT_SYMBOL(ringbuf_read);

size_t ringbuf_peek(const struct ringbuf *rb, void *data, size_t len) {
  uint8_t *dst = (uint8_t *) data;
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
EXPORT_SYMBOL(ringbuf_peek);

void ringbuf_skip(struct ringbuf *rb, size_t len) {
  size_t used = ringbuf_used(rb);
  if (len > used)
    len = used;
  rb->tail = (rb->tail + len) % rb->size;
}
EXPORT_SYMBOL(ringbuf_skip);

void ringbuf_reset(struct ringbuf *rb) {
  rb->head = 0;
  rb->tail = 0;
}

EXPORT_SYMBOL(ringbuf_reset);
