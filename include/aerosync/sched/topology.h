#pragma once

#include <aerosync/sched/cpumask.h>
#include <arch/x86_64/percpu.h>

/* Per-CPU topology masks */
DECLARE_PER_CPU(struct cpumask, cpu_sibling_map); /* SMT siblings */
DECLARE_PER_CPU(struct cpumask, cpu_core_map);    /* Cores in same package */

void build_sched_domains(void);
void update_topology_masks(void);
