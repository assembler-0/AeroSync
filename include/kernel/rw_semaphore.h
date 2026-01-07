#pragma once

#include <kernel/spinlock.h>
#include <kernel/wait.h>
#include <kernel/atomic.h>

/**
 * @file include/kernel/rw_semaphore.h
 * @brief Read-Write Semaphore implementation for AeroSync
 */

struct rw_semaphore {
    atomic_t count;
    spinlock_t wait_lock;
    wait_queue_head_t wait_list;
};

#define RWSEM_UNLOCKED_VALUE 0x00000000
#define RWSEM_ACTIVE_BIAS    0x00000001
#define RWSEM_WAITING_BIAS   (-0x00010000)
#define RWSEM_ACTIVE_READ_BIAS RWSEM_ACTIVE_BIAS
#define RWSEM_ACTIVE_WRITE_BIAS (RWSEM_WAITING_BIAS + RWSEM_ACTIVE_BIAS)

int rwsem_is_locked(const struct rw_semaphore *sem);
int rwsem_is_write_locked(const struct rw_semaphore *sem);

void rwsem_init(struct rw_semaphore *sem);
void down_read(struct rw_semaphore *sem);
int down_read_trylock(struct rw_semaphore *sem);
void up_read(struct rw_semaphore *sem);
void down_write(struct rw_semaphore *sem);
int down_write_trylock(struct rw_semaphore *sem);
void up_write(struct rw_semaphore *sem);
void downgrade_write(struct rw_semaphore *sem);
