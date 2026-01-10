/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/completion.h
 * @brief Completion synchronization primitive
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#pragma once

#include <aerosync/wait.h>

/**
 * struct completion - Structure to wait for a completion
 * @done: Completion counter (0 = not done)
 * @wait: Wait queue
 */
struct completion {
  unsigned int done;
  wait_queue_head_t wait;
};

/**
 * COMPLETION_INITIALIZER - Static initializer
 */
#define COMPLETION_INITIALIZER(work)                                           \
  {0, __WAIT_QUEUE_HEAD_INITIALIZER((work).wait)}

/**
 * DECLARE_COMPLETION - Declare and initialize a completion
 */
#define DECLARE_COMPLETION(work)                                               \
  struct completion work = COMPLETION_INITIALIZER(work)

/**
 * init_completion - Initialize a completion dynamically
 */
static inline void init_completion(struct completion *x) {
  x->done = 0;
  init_waitqueue_head(&x->wait);
}

/**
 * wait_for_completion - Wait for completion to be signaled
 */
void wait_for_completion(struct completion *x);

/**
 * wait_for_completion_timeout - Wait with timeout
 */
unsigned long wait_for_completion_timeout(struct completion *x,
                                          unsigned long timeout);

/**
 * complete - Signal completion
 */
void complete(struct completion *x);

/**
 * complete_all - Signal all waiters
 */
void complete_all(struct completion *x);
