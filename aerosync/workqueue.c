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
#include <mm/slub.h>
#include <lib/printk.h>
#include <aerosync/wait.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>

static struct workqueue_struct *system_wq;

static int __no_cfi worker_thread(void *data) {
  struct workqueue_struct *wq = data;

  printk(KERN_DEBUG KERN_CLASS "workqueue worker thread started: %s\n", wq->name);

  while (1) {
    wait_event(wq->wait, !list_empty(&wq->worklist));

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

static int ensure_worker_thread(struct workqueue_struct *wq) {
  if (atomic_read(&wq->worker_created))
    return 0;

  if (atomic_cmpxchg(&wq->worker_created, 0, 1) != 0)
    return 0;

  printk(KERN_DEBUG KERN_CLASS "Creating workqueue worker: %s\n", wq->name);

  wq->worker = kthread_create(worker_thread, wq, "wq/%s", wq->name);
  if (!wq->worker) {
    atomic_set(&wq->worker_created, 0);
    return -ENOMEM;
  }

  wq->worker->flags |= PF_WQ_WORKER;
  kthread_run(wq->worker);

  printk(KERN_DEBUG KERN_CLASS "Workqueue worker created: %s\n", wq->name);
  return 0;
}

struct workqueue_struct *create_workqueue(const char *name) {
  struct workqueue_struct *wq = kzalloc(sizeof(struct workqueue_struct));
  if (!wq) return nullptr;

  wq->name = name;
  INIT_LIST_HEAD(&wq->worklist);
  spinlock_init(&wq->lock);
  init_waitqueue_head(&wq->wait);
  atomic_set(&wq->worker_created, 0);

  return wq;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
  if (__atomic_test_and_set(&work->flags, __ATOMIC_ACQUIRE)) {
    return false; // Already pending
  }

  if (ensure_worker_thread(wq) < 0) {
    __atomic_clear(&work->flags, __ATOMIC_RELEASE);
    return false;
  }

  irq_flags_t flags = spinlock_lock_irqsave(&wq->lock);
  list_add_tail(&work->entry, &wq->worklist);
  spinlock_unlock_irqrestore(&wq->lock, flags);

  smp_mb();
  wake_up(&wq->wait);
  return true;
}

bool schedule_work(struct work_struct *work) {
  return queue_work(system_wq, work);
}

int workqueue_init(void) {
  printk(KERN_DEBUG KERN_CLASS "Initializing workqueue subsystem (lazy mode)\n");

  system_wq = create_workqueue("system");
  if (!system_wq) {
    return -ENOMEM;
  }

  printk(KERN_INFO KERN_CLASS "System workqueue initialized (worker will be created on first use).\n");
  return 0;
}
