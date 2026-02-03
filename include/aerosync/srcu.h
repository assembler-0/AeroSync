/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>
#include <aerosync/percpu.h>

/**
 * struct srcu_struct - Sleepable RCU structure
 */
struct srcu_struct {
  atomic_t completed;
  atomic_t srcu_idx;
  atomic_t lock_count[2][MAX_CPUS];
  atomic_t unlock_count[2][MAX_CPUS];
  spinlock_t lock;
  wait_queue_head_t wait;
};

#define DEFINE_SRCU(name) \
  struct srcu_struct name

int init_srcu_struct(struct srcu_struct *ssp);
void cleanup_srcu_struct(struct srcu_struct *ssp);
int srcu_read_lock(struct srcu_struct *ssp);
void srcu_read_unlock(struct srcu_struct *ssp, int idx);
void synchronize_srcu(struct srcu_struct *ssp);
void srcu_barrier(struct srcu_struct *ssp);

static inline int srcu_read_lock_held(struct srcu_struct *ssp) {
  /* Simple implementation for now */
  return 1;
}
