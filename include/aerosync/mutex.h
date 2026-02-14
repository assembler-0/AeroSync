#pragma once

#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <aerosync/wait.h>

/**
 * @file include/aerosync/mutex.h
 * @brief Mutex (Mutual Exclusion) primitive
 *
 * Mutexes are sleeping locks. When a task attempts to acquire a mutex that is
 * already held, it will sleep until the mutex is released.
 */

struct mutex {
  spinlock_t lock; /* Spinlock to protect mutex state */
  int count; /* 0 = locked, 1 = unlocked */
  struct task_struct *owner;
  wait_queue_head_t wait_q;
  
  /* Priority Inheritance fields */
  struct list_head waiters; /* List of tasks waiting on this mutex, sorted by priority */
  bool pi_enabled;
};

typedef struct mutex mutex_t;

#define MUTEX_INITIALIZER(name)                                                \
  {.lock = 0,                                                                  \
   .count = 1,                                                                 \
   .owner = nullptr,                                                              \
   .wait_q = __WAIT_QUEUE_HEAD_INITIALIZER(name.wait_q),                       \
   .waiters = LIST_HEAD_INIT(name.waiters),                                    \
   .pi_enabled = true}

#define DEFINE_MUTEX(name) mutex_t name = MUTEX_INITIALIZER(name)

#define DEFINE_MUTEX_PI(name) mutex_t name = MUTEX_INITIALIZER(name)

/**
 * Initialize a mutex
 * @param m Mutex to initialize
 */
void mutex_init(mutex_t *m);

/**
 * Lock a mutex (blocks if already held)
 * @param m Mutex to lock
 */
void mutex_lock(mutex_t *m);

/**
 * Unlock a mutex (wakes up one waiting task)
 * @param m Mutex to unlock
 */
void mutex_unlock(mutex_t *m);

/**
 * Attempt to lock a mutex without blocking
 * @param m Mutex to try locking
 * @return 1 if locked, 0 if already held
 */
int mutex_trylock(mutex_t *m);

/**
 * Check if a mutex is held by anyone
 * @param m Mutex to check
 * @return 1 if held, 0 if free
 */
static inline int mutex_is_locked(mutex_t *m) { return m->count == 0; }