#pragma once

#include <compiler.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/atomic.h>

/**
 * @file include/aerosync/mcs.h
 * @brief MCS (Mellor-Crummey and Scott) lock implementation
 * 
 * MCS locks are highly scalable spinlocks where each CPU spins on its
 * own local node, reducing cache-line bouncing.
 */

struct mcs_lock_node {
  struct mcs_lock_node *next;
  bool locked;
};

typedef struct mcs_lock_node *mcs_lock_t;

#define MCS_LOCK_INIT nullptr

static inline void mcs_lock_init(mcs_lock_t *lock) {
  *lock = MCS_LOCK_INIT;
}

/**
 * mcs_spin_lock - Acquire MCS lock
 * @lock: Pointer to the global lock head
 * @node: Pointer to per-CPU local node
 */
static inline void mcs_spin_lock(mcs_lock_t *lock, struct mcs_lock_node *node) {
  node->next = nullptr;
  node->locked = true;

  struct mcs_lock_node *prev = (struct mcs_lock_node *)xchg(lock, node);
  if (likely(prev == nullptr)) {
    return;
  }

  WRITE_ONCE(prev->next, node);

  /* Spin on local node */
  while (READ_ONCE(node->locked)) {
    cpu_relax();
  }
}

/**
 * mcs_spin_unlock - Release MCS lock
 * @lock: Pointer to the global lock head
 * @node: Pointer to per-CPU local node
 */
static inline void mcs_spin_unlock(mcs_lock_t *lock, struct mcs_lock_node *node) {
  if (likely(READ_ONCE(node->next) == nullptr)) {
    struct mcs_lock_node *expected = node;
    if (try_cmpxchg(lock, &expected, nullptr)) {
      return;
    }
    /* Wait for the next node to link itself */
    while (READ_ONCE(node->next) == nullptr) {
      cpu_relax();
    }
  }

  /* Pass the lock to the next node in queue */
  WRITE_ONCE(node->next->locked, false);
}
