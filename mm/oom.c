/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/oom.c
 * @brief Out-Of-Memory Killer
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sched/sched.h>
#include <aerosync/resdomain.h>
#include <aerosync/signal.h>
#include <aerosync/sched/process.h>
#include <lib/printk.h>
#include <mm/vma.h>
#include <aerosync/errno.h>

/*
 * Calculate "badness" score for a task.
 * Higher score = better victim.
 */
static unsigned long oom_badness(struct task_struct *p, struct resdomain *rd) {
  if (p->flags & PF_KTHREAD) return 0;
  if (p->state == TASK_DEAD || p->state == TASK_ZOMBIE) return 0;

  unsigned long points = 0;
  if (p->mm) {
    /* 
     * Base score is RSS (Resident Set Size).
     * We want to kill the process using the most physical memory.
     */
    points = (unsigned long) atomic64_read(&p->mm->rss);

    /* Also consider total VM size, but with lower weight */
    points += p->mm->total_vm / 100;
  }

  /* 
   * Penalty for processes in the offending ResDomain (or its children).
   * This localizes the OOM impact.
   */
  if (rd && resdomain_is_descendant(rd, p->rd)) {
    points *= 2;
  }

  /* Root init process is almost immune */
  if (p->pid == 1) {
    points /= 8;
  }

  return points;
}

static struct task_struct *select_bad_process(struct resdomain *rd) {
  struct task_struct *p;
  struct task_struct *victim = nullptr;
  unsigned long max_points = 0;

  // Acquire tasklist lock
  irq_flags_t flags = spinlock_lock_irqsave(&tasklist_lock);

  list_for_each_entry(p, &task_list, tasks) {
    /* Don't kill kernel threads or dying processes */
    if (p->flags & PF_KTHREAD) continue;
    if (p->state == TASK_DEAD || p->state == TASK_ZOMBIE) continue;

    unsigned long points = oom_badness(p, rd);
    if (points > max_points) {
      max_points = points;
      victim = p;
    }
  }

  spinlock_unlock_irqrestore(&tasklist_lock, flags);
  return victim;
}

/**
 * oom_kill_process - Trigger the OOM killer
 * @rd: The ResDomain that exceeded its limit (nullptr for system-wide OOM)
 */
void oom_kill_process(struct resdomain *rd) {
  struct task_struct *victim = select_bad_process(rd);

  unmet_cond_crit_else(!victim) {
    printk(KERN_ERR VMM_CLASS "oom: Killing process %d (%s) in ResDomain '%s' score %lu\n",
           victim->pid, victim->comm, victim->rd ? victim->rd->name : "none",
           oom_badness(victim, rd));

    /* Send SIGKILL to the victim */
    send_signal(SIGKILL, victim);

    /* 
     * If the victim is currently running on another CPU, it will be 
     * terminated upon its next return to userspace or next preemption point.
     */
    set_need_resched();
  }
}
