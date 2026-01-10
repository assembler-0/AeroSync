/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/lib/id_alloc.h
 * @brief Generic ID Allocator (IDA)
 * @copyright (C) 2025 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>

/**
 * struct ida - ID allocator
 */
struct ida {
    unsigned long *bitmap;
    int max_id;
    int last_id;
    spinlock_t lock;
};

/**
 * ida_init - Initialize an ID allocator
 * @ida: The IDA to initialize
 * @max_id: Maximum ID allowed
 */
void ida_init(struct ida *ida, int max_id);

/**
 * ida_alloc - Allocate an ID
 * @ida: The IDA to allocate from
 *
 * Returns the allocated ID, or -1 if no IDs are available.
 */
int ida_alloc(struct ida *ida);

/**
 * ida_alloc_min - Allocate an ID with a minimum value
 * @ida: The IDA to allocate from
 * @min: Minimum ID value
 *
 * Returns the allocated ID, or -1 if no IDs are available.
 */
int ida_alloc_min(struct ida *ida, int min);

/**
 * ida_free - Free an ID
 * @ida: The IDA to free from
 * @id: The ID to free
 */
void ida_free(struct ida *ida, int id);
