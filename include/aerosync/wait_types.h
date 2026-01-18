#pragma once

#include <aerosync/spinlock.h>
#include <linux/list.h>

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
