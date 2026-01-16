/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/writeback.c
 * @brief Dirty page writeback and throttling logic
 * @copyright (C) 2025 assembler-0
 */

#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/slab.h>
#include <aerosync/wait.h>
#include <aerosync/sched/process.h>
#include <lib/printk.h>
#include <aerosync/errno.h>
#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <linux/list.h>
#include <linux/container_of.h>
#include <aerosync/classes.h>

/* Global Dirty Page Tracking */
static LIST_HEAD(dirty_objects);
static spinlock_t dirty_lock = 0;
static DECLARE_WAIT_QUEUE_HEAD(writeback_wait);

/* Accounting */
static atomic_t nr_dirty_pages = ATOMIC_INIT(0);
#define DIRTY_THRESHOLD_WAKEUP 1024  /* 4MB dirty -> start writeback */
#define DIRTY_THRESHOLD_THROTTLE 8192 /* 32MB dirty -> throttle processes */

/**
 * account_page_dirtied - Increments global dirty page count.
 */
void account_page_dirtied(void) {
  atomic_inc(&nr_dirty_pages);
  if (atomic_read(&nr_dirty_pages) > DIRTY_THRESHOLD_WAKEUP) {
    wake_up(&writeback_wait);
  }
}

/**
 * account_page_cleaned - Decrements global dirty page count.
 */
void account_page_cleaned(void) {
  atomic_dec(&nr_dirty_pages);
}

/**
 * vm_object_mark_dirty - Adds an object to the global writeback list.
 */
void vm_object_mark_dirty(struct vm_object *obj) {
  if (!obj) return;

  irq_flags_t flags = spinlock_lock_irqsave(&dirty_lock);
  if (!(obj->flags & VM_OBJECT_DIRTY)) {
    obj->flags |= VM_OBJECT_DIRTY;
    list_add_tail(&obj->dirty_list, &dirty_objects);
    vm_object_get(obj); // Reference for the list
  }
  spinlock_unlock_irqrestore(&dirty_lock, flags);
}

/**
 * wakeup_writeback - Manually wake the writeback daemon.
 */
void wakeup_writeback(void) {
  wake_up(&writeback_wait);
}

/**
 * balance_dirty_pages - Throttle the caller if too much memory is dirty.
 * Professional kernels call this during every write() syscall.
 */
void balance_dirty_pages(struct vm_object *obj) {
  (void) obj;
  while (atomic_read(&nr_dirty_pages) > DIRTY_THRESHOLD_THROTTLE) {
    /*
     * PRODUCTION NOTE: In a real SMP system, we would sleep on a specific
     * throttle queue. For now, we yield to allow writeback to work.
     */
    schedule();
  }
}

/**
 * writeback_object - Writes out all dirty pages in a single VM Object.
 */
static void writeback_object(struct vm_object *obj) {
  if (!obj || !obj->ops || !obj->ops->write_folio) return;

  down_write(&obj->lock);

  struct rb_node *node;
  for (node = rb_first(&obj->page_tree); node; node = rb_next(node)) {
    struct folio *folio = rb_entry(node, struct folio, rb_node);

    if (folio->page.flags & PG_dirty) {
      /*
       * Perform actual I/O.
       * Note: In a production driver, write_folio should be asynchronous,
       * but here we call it synchronously.
       */
      int ret = obj->ops->write_folio(obj, folio);
      if (ret == 0) {
        folio->page.flags &= ~PG_dirty;
        account_page_cleaned();
      }
    }
  }

  up_write(&obj->lock);
}

/**
 * kwritebackd - The background daemon that cleans pages.
 */
static int kwritebackd(void *data) {
  (void) data;
  printk(KERN_INFO WRITEBACK_CLASS "kwritebackd started\n");

  while (1) {
    wait_event_interruptible(&writeback_wait,
                             !list_empty(&dirty_objects));

    irq_flags_t flags = spinlock_lock_irqsave(&dirty_lock);
    while (!list_empty(&dirty_objects)) {
      struct vm_object *obj = list_first_entry(&dirty_objects, struct vm_object, dirty_list);
      list_del_init(&obj->dirty_list);
      obj->flags &= ~VM_OBJECT_DIRTY;
      spinlock_unlock_irqrestore(&dirty_lock, flags);

      writeback_object(obj);
      vm_object_put(obj); // Release list reference

      flags = spinlock_lock_irqsave(&dirty_lock);
    }
    spinlock_unlock_irqrestore(&dirty_lock, flags);
  }

  return 0;
}

void vm_writeback_init(void) {
  struct task_struct *t = kthread_create(kwritebackd, NULL, "kwritebackd");
  if (t) {
    kthread_run(t);
  }
}
