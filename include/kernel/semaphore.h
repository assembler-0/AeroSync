#pragma once

#include <kernel/wait.h>
#include <kernel/spinlock.h>

/*
 * Semaphore implementation using wait queues for VoidFrameX kernel
 */

struct semaphore {
    spinlock_t lock;
    int count;
    wait_queue_head_t wait_list;
};

#define SEMAPHORE_INITIALIZER(name, n) \
{ \
    .lock = 0, \
    .count = n, \
    .wait_list = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait_list) \
}

#define DEFINE_SEMAPHORE(name) \
    struct semaphore name = SEMAPHORE_INITIALIZER(name, 1)

static inline void sema_init(struct semaphore *sem, int val)
{
    spinlock_init(&sem->lock);
    sem->count = val;
    init_waitqueue_head(&sem->wait_list);
}

static inline int down_trylock(struct semaphore *sem)
{
    unsigned long flags;
    int result = 1; // Return 1 if we failed to get the semaphore

    flags = spinlock_lock_irqsave(&sem->lock);
    if (sem->count > 0) {
        sem->count--;
        result = 0; // Return 0 if we got the semaphore
    }
    spinlock_unlock_irqrestore(&sem->lock, flags);

    return result;
}

static inline void down(struct semaphore *sem)
{
    wait_queue_t wait;
    init_wait(&wait);

    while (1) {
        // Try to acquire the semaphore
        if (!down_trylock(sem))
            break; // Got it!

        // Add ourselves to the wait queue and sleep
        prepare_to_wait(&sem->wait_list, &wait, TASK_UNINTERRUPTIBLE);
        
        // Check again in case someone woke us up
        if (!down_trylock(sem))
            break;

        // Now we actually sleep
        schedule();
    }
    
    finish_wait(&sem->wait_list, &wait);
}

static inline int down_interruptible(struct semaphore *sem)
{
    wait_queue_t wait;
    init_wait(&wait);
    int ret = 0;

    while (1) {
        // Try to acquire the semaphore
        if (!down_trylock(sem))
            break; // Got it!

        // Add ourselves to the wait queue and sleep
        prepare_to_wait(&sem->wait_list, &wait, TASK_INTERRUPTIBLE);
        
        // Check again in case someone woke us up
        if (!down_trylock(sem))
            break;

        // Now we actually sleep
        schedule();

        // Check if we were interrupted
        if (get_current()->state == TASK_RUNNING) {
            ret = -1; // Interrupted
            break;
        }
    }
    
    finish_wait(&sem->wait_list, &wait);
    return ret;
}

static inline void up(struct semaphore *sem)
{
    unsigned long flags;
    
    flags = spinlock_lock_irqsave(&sem->lock);
    
    sem->count++;
    
    // Wake up one waiting task if there are any
    if (sem->count <= 0) {
        wake_up(&sem->wait_list);
    } else {
        // Wake up all potentially waiting tasks
        wake_up(&sem->wait_list);
    }
    
    spinlock_unlock_irqrestore(&sem->lock, flags);
}