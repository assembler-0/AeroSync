///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/workqueue.c
 * @brief Workqueue implementation for asynchronous kernel tasks
 */

#include <aerosync/workqueue.h>
#include <aerosync/sched/sched.h>
#include <aerosync/sched/process.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <aerosync/wait.h>
#include <aerosync/classes.h>
#include <aerosync/sysintf/panic.h>

static struct workqueue_struct *system_wq;

static int worker_thread(void *data) {
  struct workqueue_struct *wq = data;

  while (1) {
    wait_event(&wq->wait, !list_empty(&wq->worklist));

    irq_flags_t flags = spinlock_lock_irqsave(&wq->lock);
    while (!list_empty(&wq->worklist)) {
      struct work_struct *work = list_first_entry(&wq->worklist, struct work_struct, entry);
      list_del_init(&work->entry);
      __atomic_and_fetch(&work->flags, ~WORK_STRUCT_PENDING, __ATOMIC_RELEASE);

      spinlock_unlock_irqrestore(&wq->lock, flags);

      if (work->func) {
        work->func(work);
      }

      flags = spinlock_lock_irqsave(&wq->lock);
    }
    spinlock_unlock_irqrestore(&wq->lock, flags);
  }
  return 0;
}

struct workqueue_struct *create_workqueue(const char *name) {
  struct workqueue_struct *wq = kzalloc(sizeof(struct workqueue_struct));
  if (!wq) return NULL;

  wq->name = name;
  INIT_LIST_HEAD(&wq->worklist);
  spinlock_init(&wq->lock);
  init_waitqueue_head(&wq->wait);

  wq->worker = kthread_create(worker_thread, wq, "wq/%s", name);
  if (!wq->worker) {
    kfree(wq);
    return NULL;
  }

  wq->worker->flags |= PF_WQ_WORKER;
  kthread_run(wq->worker);

  return wq;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  if (__atomic_test_and_set(&work->flags, __ATOMIC_ACQUIRE)) {
    return false; // Already pending
  }

  irq_flags_t flags = spinlock_lock_irqsave(&wq->lock);
  list_add_tail(&work->entry, &wq->worklist);
  spinlock_unlock_irqrestore(&wq->lock, flags);

  wake_up(&wq->wait);
  return true;
}

bool schedule_work(struct work_struct *work) {
  return queue_work(system_wq, work);
}

void workqueue_init(void) {
  system_wq = create_workqueue("system");
  if (!system_wq) {
    panic("Failed to create system workqueue");
  }
  printk(KERN_INFO KERN_CLASS "System workqueue initialized.\n");
}
