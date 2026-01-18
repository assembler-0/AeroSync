/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/topology.c
 * @brief Scheduler topology and domains (Stub)
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#include <aerosync/sched/sched.h>
#include <aerosync/sched/cpumask.h>
#include <arch/x86_64/smp.h>
#include <mm/slub.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

/*
 * Scheduling Domain Hierarchy implementation
 */

static struct sched_domain *alloc_sd(const char *name) {
  struct sched_domain *sd = kzalloc(sizeof(struct sched_domain));
  if (sd) sd->name = (char *) name;
  return sd;
}

static struct sched_group *alloc_sg(void) {
  return kzalloc(sizeof(struct sched_group));
}

/*
 * SRAT/NUMA topology helpers
 */
extern int numa_enabled;

extern int cpu_to_node(int cpu);

extern const struct cpumask *cpumask_of_node(int node);

extern int nr_node_ids;

/**
 * build_sched_domains - Construct the topology hierarchy
 *
 * Hierarchy:
 * Level 0: SMT (logical threads sharing a core)
 * Level 1: MC (Cores sharing a package/L3)
 * Level 2: NUMA (Processors across NUMA nodes) - Optional
 */
void build_sched_domains(void) {
  int nr_cpus = smp_get_cpu_count();

  printk(KERN_INFO SCHED_CLASS "Building scheduling domains for %d CPUs...\n", nr_cpus);

  for (int i = 0; i < nr_cpus; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    struct sched_domain *sd_child = NULL;

    /* 1. Build SMT Domain (Threads on the same core) */
    struct sched_domain *sd_smt = alloc_sd("SMT");
    cpumask_set_cpu(i, &sd_smt->span); /* TODO: Real HT sibling detection */

    struct sched_group *sg_smt = alloc_sg();
    cpumask_set_cpu(i, &sg_smt->cpumask);
    sg_smt->group_weight = 1;
    sd_smt->groups = sg_smt;

    sd_smt->min_interval = 1;
    sd_smt->max_interval = 4;
    sd_smt->flags = SD_LOAD_BALANCE | SD_SHARE_PKG_RESOURCES | SD_BALANCE_NEWIDLE;

    sd_child = sd_smt;
    rq->sd = sd_smt;

    /* 2. Build MC Domain (Cores in the same package/node) */
    /* If NUMA is enabled, MC domain covers the local node. Else all CPUs. */
    struct sched_domain *sd_mc = alloc_sd("MC");

    if (numa_enabled) {
      int node = cpu_to_node(i);
      const struct cpumask *node_mask = cpumask_of_node(node);
      cpumask_copy(&sd_mc->span, node_mask);
    } else {
      cpumask_setall(&sd_mc->span);
    }

    /* MC groups: each group is one SMT domain (CPU) */
    struct sched_group *head = NULL, *prev = NULL;
    int cpu;
    for_each_cpu(cpu, &sd_mc->span) {
      /* In a real kernel, we'd link SMT groups here.
         For now, creating a group per CPU is functionally equivalent for MC level
         if we assume SMT=1 or simplify. */
      struct sched_group *sg = alloc_sg();
      cpumask_set_cpu(cpu, &sg->cpumask);
      sg->group_weight = 1;

      if (!head) head = sg;
      if (prev) prev->next = sg;
      prev = sg;
    }
    /* Close the ring */
    if (prev) prev->next = head;

    sd_mc->groups = head;
    sd_mc->min_interval = 4;
    sd_mc->max_interval = 16;
    sd_mc->flags = SD_LOAD_BALANCE | SD_BALANCE_NEWIDLE;

    /* Link SMT -> MC */
    sd_child->parent = sd_mc;
    sd_mc->child = sd_child;
    sd_child = sd_mc;

    /* 3. Build NUMA Domain (Across nodes) */
    if (numa_enabled && nr_node_ids > 1) {
      struct sched_domain *sd_numa = alloc_sd("NUMA");
      cpumask_setall(&sd_numa->span);

      /* NUMA groups: Each group is a NUMA node */
      struct sched_group *n_head = NULL, *n_prev = NULL;
      for (int n = 0; n < nr_node_ids; n++) {
        struct sched_group *sg = alloc_sg();
        const struct cpumask *nm = cpumask_of_node(n);
        cpumask_copy(&sg->cpumask, nm);
        sg->group_weight = cpumask_weight(nm);

        if (!n_head) n_head = sg;
        if (n_prev) n_prev->next = sg;
        n_prev = sg;
      }
      /* Close the ring */
      if (n_prev) n_prev->next = n_head;

      sd_numa->groups = n_head;
      sd_numa->min_interval = 32; /* Slower balance across nodes */
      sd_numa->max_interval = 128;
      sd_numa->flags = SD_LOAD_BALANCE | SD_NUMA;

      /* Link MC -> NUMA */
      sd_child->parent = sd_numa;
      sd_numa->child = sd_child;
    }
  }

  printk(KERN_INFO SCHED_CLASS "Sched domains built: SMT -> MC %s\n",
         (numa_enabled && nr_node_ids > 1) ? "-> NUMA" : "");
}
