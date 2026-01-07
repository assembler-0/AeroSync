/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file kernel/sched/stats.c
 * @brief Scheduler statistics
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <arch/x86_64/percpu.h>
#include <arch/x86_64/smp.h>
#include <kernel/sched/sched.h>
#include <lib/printk.h>

void sched_show_stats(void) {
  int cpu;
  unsigned long total_switches = 0;
  unsigned long total_migrations = 0;

  printk(KERN_INFO "Scheduler Statistics:\n");

  for_each_online_cpu(cpu) {
    struct rq *rq = per_cpu_ptr(runqueues, cpu);
    struct rq_stats *stats = &rq->stats;

    printk(KERN_INFO "CPU %d: running=%u load=%lu switches=%llu mig=%llu "
                     "bal=%llu exec=%llu ns\n",
           cpu, rq->nr_running, rq->avg_load, stats->nr_switches,
           stats->nr_migrations, stats->nr_load_balance, stats->exec_clock);

    total_switches += stats->nr_switches;
    total_migrations += stats->nr_migrations;
  }

  printk(KERN_INFO "Total: switches=%lu migrations=%lu\n", total_switches,
         total_migrations);
}

void sched_debug_task(struct task_struct *p) {
  printk(KERN_INFO "Task %d (%s) state=%ld cpu=%d prio=%d/%d policy=%d\n",
         p->pid, p->comm, p->state, p->cpu, p->prio, p->normal_prio, p->policy);

  printk(KERN_INFO "  se.vruntime=%llu se.load=%lu rt.prio=%u\n",
         p->se.vruntime, p->se.load.weight, p->rt_priority);
}
