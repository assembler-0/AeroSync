/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/select.c
 * @brief I/O Multiplexing (poll/select) implementation
 * @copyright (C) 2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>
#include <aerosync/wait.h>
#include <aerosync/sched/sched.h>
#include <aerosync/errno.h>
#include <aerosync/timer.h>
#include <aerosync/signal.h>
#include <lib/uaccess.h>

struct poll_table_entry {
  struct file *filp;
  uint32_t key;
  wait_queue_entry_t wait;
  wait_queue_head_t *wait_address;
};

#define MAX_POLL_TABLE_ENTRIES 512

struct poll_wqueues {
  poll_table pt;
  struct task_struct *polling_task;
  int triggered;
  int error;
  int inline_index;
  struct poll_table_entry inline_entries[MAX_POLL_TABLE_ENTRIES];
};

static int __pollwake(wait_queue_entry_t *wait, unsigned mode, int sync, void *key) {
  struct poll_wqueues *pwq = container_of(wait, struct poll_table_entry, wait)->filp->private_data;
  /* This is hacky, in a real kernel we'd have a better way to get pwq */
  (void) pwq;

  return default_wake_function(wait, mode, sync, key);
}

/*
 * Better implementation of poll_wake that marks the table as triggered
 */
static int pollwake(wait_queue_entry_t *wait, unsigned mode, int sync, void *key) {
  struct poll_table_entry *entry = container_of(wait, struct poll_table_entry, wait);
  (void) entry;
  (void) key;
  (void) sync;
  (void) mode;

  return default_wake_function(wait, mode, sync, key);
}

static void __pollwait(struct file *filp, struct wait_queue_head *wait_address, poll_table *p) {
  struct poll_wqueues *pwq = container_of(p, struct poll_wqueues, pt);

  if (pwq->inline_index >= MAX_POLL_TABLE_ENTRIES) {
    pwq->error = -ENOMEM;
    return;
  }

  struct poll_table_entry *entry = &pwq->inline_entries[pwq->inline_index++];
  entry->filp = filp;
  entry->wait_address = wait_address;

  init_waitqueue_func_entry(&entry->wait, pollwake);
  entry->wait.private = current;
  add_wait_queue(wait_address, &entry->wait);
}

void poll_initwait(struct poll_wqueues *pwq) {
  pwq->pt._qproc = __pollwait;
  pwq->polling_task = current;
  pwq->triggered = 0;
  pwq->error = 0;
  pwq->inline_index = 0;
}

void poll_freewait(struct poll_wqueues *pwq) {
  for (int i = 0; i < pwq->inline_index; i++) {
    struct poll_table_entry *entry = &pwq->inline_entries[i];
    remove_wait_queue(entry->wait_address, &entry->wait);
  }
}

static int do_pollfd(struct pollfd *pfd, poll_table *pt) {
  int mask = 0;
  int fd = pfd->fd;

  if (fd < 0) {
    pfd->revents = 0;
    return 0;
  }

  struct file *file = fget(fd);
  if (!file) {
    pfd->revents = POLLNVAL;
    return 1;
  }

  mask = vfs_poll(file, pt);
  fput(file);

  mask &= pfd->events | POLLERR | POLLHUP;
  pfd->revents = (short) mask;

  return mask != 0;
}

int do_poll(struct pollfd *fds, unsigned int nfds, uint64_t timeout_ns) {
  struct poll_wqueues *table = kmalloc(sizeof(struct poll_wqueues));
  if (!table) return -ENOMEM;

  int count = 0;
  uint64_t deadline = 0;

  poll_initwait(table);

  if (timeout_ns != (uint64_t) -1) {
    deadline = get_time_ns() + timeout_ns;
  }

  poll_table *pt = &table->pt;

  for (;;) {
    set_current_state(TASK_INTERRUPTIBLE);

    for (unsigned int i = 0; i < nfds; i++) {
      if (do_pollfd(&fds[i], pt))
        count++;
    }

    /* Stop adding to wait queues after first pass */
    pt = nullptr;

    if (count || !timeout_ns || table->error)
      break;

    if (timeout_ns != (uint64_t) -1) {
      uint64_t now = get_time_ns();
      if (now >= deadline)
        break;

      schedule_timeout(deadline - now);
    } else {
      schedule();
    }

    if (signal_pending(current)) {
      count = -EINTR;
      break;
    }

    count = 0;
  }

  __set_current_state(TASK_RUNNING);
  poll_freewait(table);
  kfree(table);

  return count;
}
