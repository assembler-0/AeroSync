/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/srcu.c
 * @brief Sleepable Read-Copy-Update (SRCU) implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/srcu.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/smp.h>
#include <lib/string.h>

int init_srcu_struct(struct srcu_struct *ssp) {
  memset(ssp, 0, sizeof(*ssp));
  atomic_set(&ssp->completed, 0);
  atomic_set(&ssp->srcu_idx, 0);
  spinlock_init(&ssp->lock);
  init_waitqueue_head(&ssp->wait);
  return 0;
}

void cleanup_srcu_struct(struct srcu_struct *ssp) {
  /* No special cleanup needed for now */
}

int srcu_read_lock(struct srcu_struct *ssp) {
  int idx;
  
  preempt_disable();
  idx = atomic_read(&ssp->srcu_idx) & 0x1;
  atomic_inc(&ssp->lock_count[idx][smp_get_id()]);
  preempt_enable();
  
  return idx;
}

void srcu_read_unlock(struct srcu_struct *ssp, int idx) {
  preempt_disable();
  atomic_inc(&ssp->unlock_count[idx][smp_get_id()]);
  preempt_enable();
}

static bool srcu_readers_active_idx(struct srcu_struct *ssp, int idx) {
  int sum_lock = 0;
  int sum_unlock = 0;
  int cpu;
  
  for_each_possible_cpu(cpu) {
    sum_lock += atomic_read(&ssp->lock_count[idx][cpu]);
    sum_unlock += atomic_read(&ssp->unlock_count[idx][cpu]);
  }
  
  return sum_lock != sum_unlock;
}

static bool srcu_readers_active(struct srcu_struct *ssp) {
  int idx = atomic_read(&ssp->srcu_idx) & 0x1;
  return srcu_readers_active_idx(ssp, idx);
}

void synchronize_srcu(struct srcu_struct *ssp) {
  int cur_idx;
  
  spinlock_lock(&ssp->lock);
  
  /* 1. Flip the index */
  cur_idx = atomic_read(&ssp->srcu_idx);
  atomic_set(&ssp->srcu_idx, cur_idx ^ 1);
  
  /* 2. Wait for all readers of the PREVIOUS index to finish */
  /* We use cur_idx & 0x1 which is what readers used */
  while (srcu_readers_active_idx(ssp, cur_idx & 0x1)) {
    schedule();
  }
  
  /* 3. Wait for all readers of the NEW index that started before we flipped */
  /* This is a simple implementation of SRCU, Linux is more complex */
  
  atomic_inc(&ssp->completed);
  spinlock_unlock(&ssp->lock);
}

void srcu_barrier(struct srcu_struct *ssp) {
  synchronize_srcu(ssp);
}
