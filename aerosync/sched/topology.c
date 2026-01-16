/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/topology.c
 * @brief Scheduler topology and domains (Stub)
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <aerosync/sched/sched.h>
#include <aerosync/sched/cpumask.h>
#include <arch/x86_64/smp.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

/*
 * Scheduling Domain Hierarchy implementation
 */

static struct sched_domain *alloc_sd(const char *name) {
    struct sched_domain *sd = kzalloc(sizeof(struct sched_domain));
    if (sd) sd->name = (char *)name;
    return sd;
}

static struct sched_group *alloc_sg(void) {
    return kzalloc(sizeof(struct sched_group));
}

/**
 * build_sched_domains - Construct the topology hierarchy
 * 
 * This currently builds a 2-level hierarchy:
 * Level 0: SMT (logical threads sharing a core)
 * Level 1: MC (Cores sharing a package/L3)
 */
void build_sched_domains(void) {
    int nr_cpus = smp_get_cpu_count();
    
    printk(KERN_INFO SCHED_CLASS "Building scheduling domains for %d CPUs...\n", nr_cpus);

    for (int i = 0; i < nr_cpus; i++) {
        struct rq *rq = per_cpu_ptr(runqueues, i);
        
        /* 1. Build SMT Domain (Threads on the same core) */
        /* Note: For now, we assume 1 thread per core if we don't have detailed CPUID parsing */
        struct sched_domain *sd_smt = alloc_sd("SMT");
        cpumask_set_cpu(i, &sd_smt->span);
        
        struct sched_group *sg_smt = alloc_sg();
        cpumask_set_cpu(i, &sg_smt->cpumask);
        sg_smt->group_weight = 1;
        sd_smt->groups = sg_smt;
        
        sd_smt->min_interval = 1;
        sd_smt->max_interval = 4;
        sd_smt->flags = SD_LOAD_BALANCE | SD_SHARE_PKG_RESOURCES;

        /* 2. Build MC Domain (Cores in the same package) */
        struct sched_domain *sd_mc = alloc_sd("MC");
        cpumask_setall(&sd_mc->span); // All CPUs in one MC domain for now
        
        /* MC groups: each group is one SMT domain */
        struct sched_group *head = NULL, *prev = NULL;
        for (int j = 0; j < nr_cpus; j++) {
            struct sched_group *sg = alloc_sg();
            cpumask_set_cpu(j, &sg->cpumask);
            sg->group_weight = 1;
            if (!head) head = sg;
            if (prev) prev->next = sg;
            prev = sg;
        }
        sd_mc->groups = head;
        sd_mc->min_interval = 4;
        sd_mc->max_interval = 16;
        sd_mc->flags = SD_LOAD_BALANCE;

        /* Link them */
        sd_smt->parent = sd_mc;
        sd_mc->child = sd_smt;
        
        rq->sd = sd_smt;
    }

    printk(KERN_INFO SCHED_CLASS "Sched domains built: SMT -> MC\n");
}
