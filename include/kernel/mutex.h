#pragma once

#include <kernel/wait.h>
#include <kernel/spinlock.h>

/*
 * Mutex implementation using wait queues for VoidFrameX kernel
 */

struct mutex {
    spinlock_t lock;
    int owner;
    int count;
    wait_queue_head_t wait_list;
};

#define MUTEX_INITIALIZER(name) \
{ \
    .lock = 0, \
    .owner = -1, \
    .count = 1, \
    .wait_list = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait_list) \
}

#define DEFINE_MUTEX(name) \
    struct mutex name = MUTEX_INITIALIZER(name)

static inline void mutex_init(struct mutex *mutex)
{
    spinlock_init(&mutex->lock);
    mutex->owner = -1;
    mutex->count = 1;
    init_waitqueue_head(&mutex->wait_list);
}

static inline int mutex_trylock(struct mutex *mutex)
{
    unsigned long flags;
    int result = 0; // Return 0 if we failed to get the mutex

    flags = spinlock_lock_irqsave(&mutex->lock);
    if (mutex->count > 0) {
        mutex->count--;
        mutex->owner = get_current()->pid; // Store current task ID
        result = 1; // Return 1 if we got the mutex
    }
    spinlock_unlock_irqrestore(&mutex->lock, flags);

    return result;
}

static inline void mutex_lock(struct mutex *mutex)
{
    wait_queue_t wait;
    init_wait(&wait);

    while (1) {
        // Try to acquire the mutex
        if (mutex_trylock(mutex))
            break; // Got it!

        // Add ourselves to the wait queue and sleep
        prepare_to_wait(&mutex->wait_list, &wait, TASK_UNINTERRUPTIBLE);
        
        // Check again in case someone woke us up
        if (mutex_trylock(mutex))
            break;

        // Now we actually sleep
        schedule();
    }
    
    finish_wait(&mutex->wait_list, &wait);
}

static inline int mutex_lock_interruptible(struct mutex *mutex)
{
    wait_queue_t wait;
    init_wait(&wait);
    int ret = 0;

    while (1) {
        // Try to acquire the mutex
        if (mutex_trylock(mutex))
            break; // Got it!

        // Add ourselves to the wait queue and sleep
        prepare_to_wait(&mutex->wait_list, &wait, TASK_INTERRUPTIBLE);
        
        // Check again in case someone woke us up
        if (mutex_trylock(mutex))
            break;

        // Now we actually sleep
        schedule();

        // Check if we were interrupted
        if (get_current()->state == TASK_RUNNING) {
            ret = -1; // Interrupted
            break;
        }
    }
    
    finish_wait(&mutex->wait_list, &wait);
    return ret;
}

static inline void mutex_unlock(struct mutex *mutex)
{
    unsigned long flags;
    
    flags = spinlock_lock_irqsave(&mutex->lock);
    
    if (mutex->count < 1) {
        mutex->count++;
        mutex->owner = -1;
        
        // Wake up one waiting task if there are any
        wake_up(&mutex->wait_list);
    }
    
    spinlock_unlock_irqrestore(&mutex->lock, flags);
}