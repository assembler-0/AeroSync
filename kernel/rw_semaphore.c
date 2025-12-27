#include <kernel/rw_semaphore.h>
#include <kernel/sched/sched.h>
#include <kernel/atomic.h>

/**
 * Simple RW-Semaphore implementation.
 * Note: This implementation is functional but can be further optimized
 * with architecture-specific atomics if needed.
 */

void rwsem_init(struct rw_semaphore *sem) {
    atomic_set(&sem->count, RWSEM_UNLOCKED_VALUE);
    spinlock_init(&sem->wait_lock);
    init_waitqueue_head(&sem->wait_list);
}

void down_read(struct rw_semaphore *sem) {
    while (1) {
        int old = atomic_read(&sem->count);
        if (old >= 0) {
            if (atomic_cmpxchg(&sem->count, old, old + 1) == old)
                return;
        }
        
        // Wait for it to become positive
        wait_queue_t wait;
        init_wait(&wait);
        prepare_to_wait(&sem->wait_list, &wait, TASK_UNINTERRUPTIBLE);
        if (atomic_read(&sem->count) < 0) {
            schedule();
        }
        finish_wait(&sem->wait_list, &wait);
    }
}

int down_read_trylock(struct rw_semaphore *sem) {
    int old = atomic_read(&sem->count);
    if (old >= 0) {
        if (atomic_cmpxchg(&sem->count, old, old + 1) == old)
            return 1;
    }
    return 0;
}

void up_read(struct rw_semaphore *sem) {
    int old = atomic_dec_return(&sem->count);
    if (old == 0) {
        wake_up(&sem->wait_list);
    }
}

void down_write(struct rw_semaphore *sem) {
    while (1) {
        if (atomic_cmpxchg(&sem->count, 0, -1) == 0)
            return;

        wait_queue_t wait;
        init_wait(&wait);
        prepare_to_wait(&sem->wait_list, &wait, TASK_UNINTERRUPTIBLE);
        if (atomic_read(&sem->count) != 0) {
            schedule();
        }
        finish_wait(&sem->wait_list, &wait);
    }
}

int down_write_trylock(struct rw_semaphore *sem) {
    if (atomic_cmpxchg(&sem->count, 0, -1) == 0)
        return 1;
    return 0;
}

void up_write(struct rw_semaphore *sem) {
    atomic_set(&sem->count, 0);
    wake_up_all(&sem->wait_list);
}

void downgrade_write(struct rw_semaphore *sem) {
    atomic_set(&sem->count, 1);
    wake_up_all(&sem->wait_list);
}
