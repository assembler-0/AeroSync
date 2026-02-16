/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/writeback.c
 * @brief Dirty page writeback and throttling logic
 * @copyright (C) 2025-2026 assembler-0
 */

#include <mm/vm_object.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <aerosync/wait.h>
#include <aerosync/sched/process.h>
#include <arch/x86_64/mm/tlb.h>
#include <aerosync/sched/sched.h>
#include <aerosync/resdomain.h>
#include <lib/printk.h>
#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <linux/list.h>
#include <linux/container_of.h>
#include <aerosync/classes.h>

/* Global Dirty Page Tracking */
static LIST_HEAD(dirty_objects);
static DEFINE_SPINLOCK(dirty_lock);
static DECLARE_WAIT_QUEUE_HEAD(writeback_wait);

/* Accounting */
static atomic_t nr_dirty_pages = ATOMIC_INIT(0);

#ifndef DIRTY_THRESHOLD_WAKEUP
#ifndef CONFIG_DIRTY_THRESHOLD_WAKEUP
#define DIRTY_THRESHOLD_WAKEUP 1024  /* 4MB dirty -> start writeback */
#else
#define DIRTY_THRESHOLD_WAKEUP CONFIG_DIRTY_THRESHOLD_WAKEUP
#endif
#endif

#ifndef DIRTY_THRESHOLD_THROTTLE
#ifndef CONFIG_DIRTY_THRESHOLD_THROTTLE
#define DIRTY_THRESHOLD_THROTTLE 8192 /* 32MB dirty -> throttle processes */
#else
#define DIRTY_THRESHOLD_THROTTLE CONFIG_DIRTY_THRESHOLD_THROTTLE
#endif
#endif

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
  int dirty = atomic_read(&nr_dirty_pages);
  
#ifdef CONFIG_MM_DIRTY_THROTTLING
    if (dirty > DIRTY_THRESHOLD_THROTTLE) {
    /*
     * PROPORTIONAL THROTTLING:
     * If we are over the limit, sleep for a bit to allow kworker/writeback
     * to catch up. The sleep duration is proportional to the excess.
     */
    uint64_t excess = dirty - DIRTY_THRESHOLD_THROTTLE;
    uint64_t pause_ns = (excess * 1000000ULL) / (DIRTY_THRESHOLD_THROTTLE / 100 + 1);
    
    /* Apply per-task pressure factor if available */
    struct task_struct *curr = current;
    if (curr && curr->dirty_paused_ns > 0) {
        pause_ns += curr->dirty_paused_ns / 8;
        curr->dirty_paused_ns = pause_ns;
    }

    uint64_t max_pause = (uint64_t)CONFIG_MM_THROTTLE_MAX_PAUSE_MS * 1000000ULL;
    if (pause_ns > max_pause) pause_ns = max_pause;
    
    schedule_timeout(pause_ns);
  }
#endif

  if (dirty > DIRTY_THRESHOLD_WAKEUP) {
    wakeup_writeback();
  }
}

/**
 * balance_dirty_pages_ratelimited - Rate-limited version of balance_dirty_pages.
 * Use this in hot paths (like folio_mark_dirty) to avoid excessive overhead.
 */
void balance_dirty_pages_ratelimited(struct vm_object *obj) {
    static DEFINE_PER_CPU(int, dirty_count);
    int count = this_cpu_read(dirty_count);
    
    if (++count >= 32) {
        this_cpu_write(dirty_count, 0);
        balance_dirty_pages(obj);
    } else {
        this_cpu_write(dirty_count, count);
    }
}

/**
 * writeback_object - Writes out dirty pages using clustering for performance.
 */
static void writeback_object(struct vm_object *obj) {
  if (!obj || !obj->ops) return;

  down_write(&obj->lock);

  unsigned long index = 0;
  struct folio *folio;

#ifdef CONFIG_MM_UBC_CLUSTERING
  struct folio *cluster[32];
  uint32_t cluster_count = 0;

  while (xa_find(&obj->page_tree, &index, ULONG_MAX, XA_PRESENT)) {
    folio = xa_load(&obj->page_tree, index);
    if (!folio || xa_is_err(folio) || ((uintptr_t) folio & 0x1)) {
      index++;
      continue;
    }

    if (folio->page.flags & PG_dirty) {
      folio->page.flags &= ~PG_dirty;
      cluster[cluster_count++] = folio;

      unsigned long next_idx = index + 1;
      while (cluster_count < 32) {
        struct folio *next = xa_load(&obj->page_tree, next_idx);
        if (next && !xa_is_err(next) && !((uintptr_t) next & 0x1) &&
            (next->page.flags & PG_dirty)) {
          next->page.flags &= ~PG_dirty;
          cluster[cluster_count++] = next;
          next_idx++;
        } else {
          break;
        }
      }

      int ret = -1;
      if (obj->ops->write_folios) {
        ret = obj->ops->write_folios(obj, cluster, cluster_count);
      } else if (obj->ops->write_folio) {
        for (uint32_t i = 0; i < cluster_count; i++) {
          if (obj->ops->write_folio(obj, cluster[i]) == 0) {
            account_page_cleaned();
          } else {
            cluster[i]->page.flags |= PG_dirty;
          }
        }
        ret = 0;
      }

      if (ret == 0 && obj->ops->write_folios) {
        for (uint32_t i = 0; i < cluster_count; i++) account_page_cleaned();
      } else if (ret != 0 && obj->ops->write_folios) {
        for (uint32_t i = 0; i < cluster_count; i++) cluster[i]->page.flags |= PG_dirty;
      }

      cluster_count = 0;
      index = next_idx;
    } else {
      index++;
    }
#else
    xa_for_each(&obj->page_tree, index, folio) {
      if (xa_is_err(folio) || ((uintptr_t) folio & 0x1)) continue;

      if (folio->page.flags & PG_dirty) {
        folio->page.flags &= ~PG_dirty;
        if (obj->ops->write_folio && obj->ops->write_folio(obj, folio) == 0) {
          account_page_cleaned();
        } else {
          folio->page.flags |= PG_dirty;
        }
      }
#endif

    /* Yield periodically */
    if (unlikely(index % 256 == 0)) {
      up_write(&obj->lock);
      schedule();
      down_write(&obj->lock);
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
    /* Wait until there are dirty objects OR system-wide dirty pressure is high */
    wait_event_interruptible(writeback_wait,
                             !list_empty(&dirty_objects) ||
                             atomic_read(&nr_dirty_pages) > DIRTY_THRESHOLD_WAKEUP);

    irq_flags_t flags = spinlock_lock_irqsave(&dirty_lock);
    while (!list_empty(&dirty_objects)) {
      struct vm_object *obj = list_first_entry(&dirty_objects, struct vm_object, dirty_list);
      list_del_init(&obj->dirty_list);
      obj->flags &= ~VM_OBJECT_DIRTY;
      spinlock_unlock_irqrestore(&dirty_lock, flags);

      /*
       * Check if object is still valid.
       * (refcount was increased in vm_object_mark_dirty)
       */
      writeback_object(obj);
      vm_object_put(obj);

      /* Throttling and fairness: yield to other threads */
      schedule();

      flags = spinlock_lock_irqsave(&dirty_lock);
    }
    spinlock_unlock_irqrestore(&dirty_lock, flags);

    /* Additional periodic cleanup for non-listed but dirty objects if needed */
  }

  return 0;
}

int vm_writeback_init(void) {
  struct task_struct *t = kthread_create(kwritebackd, nullptr, "kwritebackd");
  if (!t)
    return -ENOMEM;
  kthread_run(t);
  return 0;
}
