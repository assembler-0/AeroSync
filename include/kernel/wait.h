#pragma once

#include <kernel/sched/sched.h>
#include <kernel/spinlock.h>
#include <linux/list.h>

/*
 * Wait queue implementation for AeroSync kernel
 * Similar to Linux wait queues but adapted for the CFS scheduler
 */

struct __wait_queue_head {
  spinlock_t lock;
  struct list_head task_list;
};

typedef struct __wait_queue_head wait_queue_head_t;

struct __wait_queue {
  unsigned int flags;
  struct task_struct *task;
  struct list_head entry;
  int (*func)(wait_queue_head_t *wq_head, struct __wait_queue *wait, int mode,
              unsigned long key);
};

typedef struct __wait_queue wait_queue_t;

/* Wait queue flags */
#define WQ_FLAG_EXCLUSIVE 0x01
#define WQ_FLAG_WOKEN 0x02

/* Wait queue initialization macros */
#define __WAIT_QUEUE_HEAD_INITIALIZER(name)                                    \
  {.lock = 0, .task_list = LIST_HEAD_INIT(name.task_list)}

#define DECLARE_WAIT_QUEUE_HEAD(name)                                          \
  wait_queue_head_t name = __WAIT_QUEUE_HEAD_INITIALIZER(name)

#define __WAITQUEUE_INITIALIZER(name, tsk)                                     \
  {.flags = 0,                                                                 \
   .task = tsk,                                                                \
   .entry = LIST_HEAD_INIT(name.entry),                                        \
   .func = default_wake_function}

/* Wait condition macros */
#define wait_event(wq, condition)                                              \
  do {                                                                         \
    if (condition)                                                             \
      break;                                                                   \
    wait_queue_t __wait;                                                       \
    init_wait(&__wait);                                                        \
    for (;;) {                                                                 \
      prepare_to_wait(wq, &__wait, TASK_UNINTERRUPTIBLE);                      \
      if (condition)                                                           \
        break;                                                                 \
      schedule();                                                              \
    }                                                                          \
    finish_wait(wq, &__wait);                                                  \
  } while (0)

#define wait_event_interruptible(wq, condition)                                \
  ({                                                                           \
    int __ret = 0;                                                             \
    if (!(condition)) {                                                        \
      wait_queue_t __wait;                                                     \
      init_wait(&__wait);                                                      \
      for (;;) {                                                               \
        prepare_to_wait(wq, &__wait, TASK_INTERRUPTIBLE);                      \
        if (condition)                                                         \
          break;                                                               \
        schedule();                                                            \
        if (get_current()->state == TASK_RUNNING && !(condition)) {            \
          __ret = -1;                                                          \
          break;                                                               \
        }                                                                      \
      }                                                                        \
      finish_wait(wq, &__wait);                                                \
    }                                                                          \
    __ret;                                                                     \
  })

#define wait_event_timeout(wq, condition_fn, data, timeout) \
({ \
    long __ret = timeout; \
    if (!(condition_fn(data))) \
        __ret = __wait_event_timeout(&wq, condition_fn, data, timeout); \
    __ret; \
})

#define wait_event_interruptible_timeout(wq, condition, timeout)               \
  ({                                                                           \
    long __ret = timeout;                                                      \
    if (!(condition))                                                          \
      __ret = __wait_event_interruptible_timeout(wq, condition, timeout);      \
    __ret;                                                                     \
  })

/* Initialization functions */
static inline void init_waitqueue_head(wait_queue_head_t *wq_head) {
  spinlock_init(&wq_head->lock);
  INIT_LIST_HEAD(&wq_head->task_list);
}

/* Wait queue functions */
void add_wait_queue(wait_queue_head_t *wq_head, wait_queue_t *wait);
void remove_wait_queue(wait_queue_head_t *wq_head, wait_queue_t *wait);

/* Default wake function */
int default_wake_function(wait_queue_head_t *wq_head, struct __wait_queue *wait,
                          int mode, unsigned long key);

/* Prepare to sleep functions */
long prepare_to_wait(wait_queue_head_t *wq_head, wait_queue_t *wait, int state);
void finish_wait(wait_queue_head_t *wq_head, wait_queue_t *wait);
extern long __wait_event_timeout(wait_queue_head_t *wq,
                                 int (*condition)(void *), void *data,
                                 long timeout);

/* Wake up functions */
void wake_up(wait_queue_head_t *wq_head);
void wake_up_nr(wait_queue_head_t *wq_head, int nr_exclusive);
void wake_up_all(wait_queue_head_t *wq_head);
void wake_up_interruptible(wait_queue_head_t *wq_head);

/* Task sleep functions */
void sleep_on(wait_queue_head_t *wq);
void interruptible_sleep_on(wait_queue_head_t *wq);

/* Helper macros for common patterns */
#define DEFINE_WAIT_FUNC(name, function)                                       \
  wait_queue_t name = {.flags = 0,                                             \
                       .task = get_current(),                                  \
                       .entry = LIST_HEAD_INIT(name.entry),                    \
                       .func = function}

#define DEFINE_WAIT(name) DEFINE_WAIT_FUNC(name, default_wake_function)

/* Initialization macro for wait queue entries */
#define init_wait(wait)                                                        \
  do {                                                                         \
    (wait)->task = get_current();                                              \
    (wait)->flags = 0;                                                         \
    INIT_LIST_HEAD(&(wait)->entry);                                            \
    (wait)->func = default_wake_function;                                      \
  } while (0)

/* Counter-based synchronization primitive */
struct wait_counter {
  wait_queue_head_t wait_q;
  volatile int count;
  volatile int target;
  spinlock_t lock;
};

static inline void init_wait_counter(struct wait_counter *wc, int initial,
                                     int target_val) {
  init_waitqueue_head(&wc->wait_q);
  wc->count = initial;
  wc->target = target_val;
  spinlock_init(&wc->lock);
}

static inline void wait_counter_inc(struct wait_counter *wc) {
  irq_flags_t flags = spinlock_lock_irqsave(&wc->lock);
  wc->count++;
  if (wc->count >= wc->target) {
    wake_up_all(&wc->wait_q);
  }
  spinlock_unlock_irqrestore(&wc->lock, flags);
}

static inline void wait_counter_wait(struct wait_counter *wc) {
  wait_queue_t wait;
  init_wait(&wait);

  while (1) {
    irq_flags_t flags = spinlock_lock_irqsave(&wc->lock);
    if (wc->count >= wc->target) {
      spinlock_unlock_irqrestore(&wc->lock, flags);
      break;
    }
    add_wait_queue(&wc->wait_q, &wait);
    spinlock_unlock_irqrestore(&wc->lock, flags);

    schedule();

    remove_wait_queue(&wc->wait_q, &wait);
  }
}