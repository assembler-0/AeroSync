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
#include <arch/x86_64/cpu.h>

/* Per-CPU topology masks */
DEFINE_PER_CPU(struct cpumask, cpu_sibling_map); /* SMT siblings */
DEFINE_PER_CPU(struct cpumask, cpu_core_map);    /* Cores in same package */

static struct sched_domain *alloc_sd(const char *name) {
  struct sched_domain *sd = kzalloc(sizeof(struct sched_domain));
  if (sd) sd->name = (char *) name;
  return sd;
}

static struct sched_group *alloc_sg(void) {
  return kzalloc(sizeof(struct sched_group));
}

/**
 * update_topology_masks - Initialize SMT and Core sibling masks
 */
void update_topology_masks(void) {
  int nr_cpus = (int)smp_get_cpu_count();
  
  for (int i = 0; i < nr_cpus; i++) {
    struct cpuinfo_x86 *ci_i = per_cpu_ptr(cpu_info, i);
    struct cpumask *sib_i = per_cpu_ptr(cpu_sibling_map, i);
    struct cpumask *core_i = per_cpu_ptr(cpu_core_map, i);
    
    cpumask_clear(sib_i);
    cpumask_clear(core_i);
    
    for (int j = 0; j < nr_cpus; j++) {
      struct cpuinfo_x86 *ci_j = per_cpu_ptr(cpu_info, j);
      
      /* Same package? */
      if (ci_i->package_id == ci_j->package_id) {
        cpumask_set_cpu(j, core_i);
        
        /* Same core? (SMT siblings) */
        if (ci_i->core_id == ci_j->core_id) {
          cpumask_set_cpu(j, sib_i);
        }
      }
    }
  }
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
  int nr_cpus = (int)smp_get_cpu_count();

  update_topology_masks();

  printk(KERN_INFO SCHED_CLASS "Building scheduling domains for %d CPUs...\n", nr_cpus);

  for (int i = 0; i < nr_cpus; i++) {
    struct rq *rq = per_cpu_ptr(runqueues, i);
    struct sched_domain *sd_child = NULL;
    struct cpuinfo_x86 *ci = per_cpu_ptr(cpu_info, i);

#ifdef CONFIG_SCHED_SMT
    /* 1. Build SMT Domain */
    struct cpumask *sib_mask = per_cpu_ptr(cpu_sibling_map, i);
    if (cpumask_weight(sib_mask) > 1) {
      struct sched_domain *sd_smt = alloc_sd("SMT");
      cpumask_copy(&sd_smt->span, sib_mask);

      struct sched_group *head = NULL, *prev = NULL;
      int cpu;
      for_each_cpu(cpu, sib_mask) {
        struct sched_group *sg = alloc_sg();
        cpumask_set_cpu(cpu, &sg->cpumask);
        sg->group_weight = 1;
        if (!head) head = sg;
        if (prev) prev->next = sg;
        prev = sg;
      }
      if (prev) prev->next = head;

      sd_smt->groups = head;
      sd_smt->min_interval = 1;
      sd_smt->max_interval = 4;
      sd_smt->flags = SD_LOAD_BALANCE | SD_SHARE_PKG_RESOURCES | SD_BALANCE_NEWIDLE;

      rq->sd = sd_smt;
      sd_child = sd_smt;
    }
#endif

    /* 2. Build MC Domain */
    struct cpumask *core_mask = per_cpu_ptr(cpu_core_map, i);
    struct sched_domain *sd_mc = alloc_sd("MC");
    cpumask_copy(&sd_mc->span, core_mask);

    struct sched_group *head = NULL, *prev = NULL;
    int cpu;
    for_each_cpu(cpu, core_mask) {
      /* Group is an SMT sibling set */
      struct cpumask *sib = per_cpu_ptr(cpu_sibling_map, cpu);
      
      /* Only add one group per core (first sibling) */
      if (cpumask_first(sib) != cpu) continue;

      struct sched_group *sg = alloc_sg();
      cpumask_copy(&sg->cpumask, sib);
      sg->group_weight = cpumask_weight(sib);

      if (!head) head = sg;
      if (prev) prev->next = sg;
      prev = sg;
    }
    if (prev) prev->next = head;

    sd_mc->groups = head;
    sd_mc->min_interval = 4;
    sd_mc->max_interval = 16;
    sd_mc->flags = SD_LOAD_BALANCE | SD_BALANCE_NEWIDLE | SD_SHARE_PKG_RESOURCES;

#ifdef CONFIG_SCHED_HYBRID
    /* If this is a hybrid system, flag for asymmetric packing */
    /* Check if there are different core types in the system */
    bool hybrid = false;
    for (int j = 0; j < nr_cpus; j++) {
        if (per_cpu_ptr(cpu_info, j)->core_type != ci->core_type) {
            hybrid = true;
            break;
        }
    }
    if (hybrid) sd_mc->flags |= SD_ASYM_PACKING;
#endif

    if (sd_child) {
      sd_child->parent = sd_mc;
      sd_mc->child = sd_child;
    } else {
      rq->sd = sd_mc;
    }
    sd_child = sd_mc;

    /* Update CPU capacity for Hybrid systems */
#ifdef CONFIG_SCHED_HYBRID
    if (ci->core_type == CORE_TYPE_INTEL_CORE) {
        rq->cpu_capacity = 1024; /* P-Core */
    } else if (ci->core_type == CORE_TYPE_INTEL_ATOM) {
        rq->cpu_capacity = 512;  /* E-Core */
    } else {
        rq->cpu_capacity = 1024;
    }
#else
    rq->cpu_capacity = 1024;
#endif

    /* 3. Build NUMA Domain */
    if (numa_enabled && nr_node_ids > 1) {
      struct sched_domain *sd_numa = alloc_sd("NUMA");
      cpumask_setall(&sd_numa->span);

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
      if (n_prev) n_prev->next = n_head;

      sd_numa->groups = n_head;
      sd_numa->min_interval = 32;
      sd_numa->max_interval = 128;
      sd_numa->flags = SD_LOAD_BALANCE | SD_NUMA;

      sd_child->parent = sd_numa;
      sd_numa->child = sd_child;
    }
  }

  printk(KERN_INFO SCHED_CLASS "Sched domains built: %sMC %s\n",
         (this_cpu_ptr(cpu_sibling_map)->bits[0] != (1UL << smp_get_id())) ? "SMT -> " : "",
         (numa_enabled && nr_node_ids > 1) ? "-> NUMA" : "");
}
