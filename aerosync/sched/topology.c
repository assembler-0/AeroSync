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
#include <lib/printk.h>
#include <aerosync/classes.h>

/*
 * Future implementation of scheduling domains for NUMA awareness.
 */

void build_sched_domains(void) {
  printk(KERN_INFO SCHED_CLASS "Topology detection (stub)\n");
}
