/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/stats.c
 * @brief Scheduler statistics and debug information
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sched/sched.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

void sched_show_stats(void) {
    printk(KERN_INFO SCHED_CLASS "scheduler Statistics:\n");
    for (int i = 0; i < smp_get_cpu_count(); i++) {
        struct rq *rq = per_cpu_ptr(runqueues, i);
        /* 
         * Lockless read of stats. 
         * Values might be slightly stale or torn (rarely), but safe for display.
         * We avoid locking all runqueues to prevent massive contention/deadlocks during debug dumps.
         */
        printk(KERN_INFO SCHED_CLASS "  CPU %d: nr_running=%u, load_avg=%lu, util_avg=%lu, switches=%llu\n",
               i, rq->nr_running, rq->cfs.avg.load_avg, rq->cfs.avg.util_avg,
               rq->stats.nr_switches);
    }
}

void sched_debug_task(struct task_struct *p) {
    if (!p) return;
    printk(KERN_DEBUG SCHED_CLASS "task %s (%d): prio=%d, vruntime=%llu, load_avg=%lu\n",
           p->comm, p->pid, p->prio, p->se.vruntime, p->se.avg.load_avg);
}