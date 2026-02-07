/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/resdomain.c
 * @brief ResDomain core implementation
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

#include <aerosync/classes.h>
#include <aerosync/resdomain.h>
#include <aerosync/sched/process.h>
#include <aerosync/errno.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <lib/printk.h>

struct resdomain root_resdomain;

void resdomain_init(void) {
  memset(&root_resdomain, 0, sizeof(root_resdomain));
  strncpy(root_resdomain.name, "root", sizeof(root_resdomain.name));
  atomic_set(&root_resdomain.refcount, 1);
  INIT_LIST_HEAD(&root_resdomain.children);
  INIT_LIST_HEAD(&root_resdomain.sibling);
  spinlock_init(&root_resdomain.lock);

  root_resdomain.cpu_weight = 1024;
  root_resdomain.mem_limit = (uint64_t) -1;

  resfs_init();
  resfs_bind_domain(&root_resdomain);

  printk(KERN_INFO SCHED_CLASS "Hierarchical Resource Domains (ResDomain) initialized\n");
}

struct resdomain *resdomain_create(struct resdomain *parent, const char *name) {
  struct resdomain *rd = kzalloc(sizeof(struct resdomain));
  if (!rd) return nullptr;

  strncpy(rd->name, name, sizeof(rd->name));
  atomic_set(&rd->refcount, 1);
  INIT_LIST_HEAD(&rd->children);
  INIT_LIST_HEAD(&rd->sibling);
  spinlock_init(&rd->lock);

  rd->parent = parent;
  rd->cpu_weight = 1024;
  rd->mem_limit = parent ? parent->mem_limit : (uint64_t) -1;

  if (parent) {
    resdomain_get(parent);
    spinlock_lock(&parent->lock);
    list_add_tail(&rd->sibling, &parent->children);
    spinlock_unlock(&parent->lock);
  }

  /* Automatically expose to userspace */
  resfs_bind_domain(rd);

  /*
   * In a full implementation, we would allocate rd->se and rd->cfs_rq here
   * using per-CPU allocators to support group scheduling.
   */

  return rd;
}

void resdomain_put(struct resdomain *rd) {
  if (!rd || rd == &root_resdomain) return;

  if (atomic_dec_and_test(&rd->refcount)) {
    if (rd->parent) {
      spinlock_lock(&rd->parent->lock);
      list_del(&rd->sibling);
      spinlock_unlock(&rd->parent->lock);
      resdomain_put(rd->parent);
    }
    kfree(rd);
  }
}

void resdomain_task_init(struct task_struct *p, struct task_struct *parent) {
  if (parent && parent->rd) {
    p->rd = parent->rd;
  } else {
    p->rd = &root_resdomain;
  }
  resdomain_get(p->rd);
}

int resdomain_attach_task(struct resdomain *rd, struct task_struct *task) {
  if (!rd || !task) return -EINVAL;

  struct resdomain *old_rd = task->rd;
  if (rd == old_rd) return 0;

  resdomain_get(rd);
  task->rd = rd;

  /* Safely update the scheduler's internal hierarchy */
  sched_move_task(task);

  if (old_rd) resdomain_put(old_rd);

  return 0;
}
