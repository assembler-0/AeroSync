/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/resdomain.h
 * @brief Unified Resource Domain (ResDomain) system
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <linux/list.h>
#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <aerosync/sched/sched.h>

struct resdomain {
    char name[64];
    atomic_t refcount;
    
    /* Hierarchy */
    struct resdomain *parent;
    struct list_head children;
    struct list_head sibling;
    
    /* Scheduler Integration (Group Scheduling) */
    struct sched_entity **se; /* Per-CPU entities for this domain */
    struct cfs_rq **cfs_rq;   /* Per-CPU runqueues for children of this domain */
    
    /* Resource Controls */
    uint32_t cpu_weight;
    uint64_t mem_limit;
    atomic64_t mem_usage;
    
    void *private_data; /* Mapping to filesystem node (e.g., pseudo_node) */

    /* Locking */
    spinlock_t lock;
};

extern struct resdomain root_resdomain;

/**
 * resdomain_init - Initialize the ResDomain subsystem
 */
void resdomain_init(void);

/**
 * resfs_init - Initialize the Resource Domain Filesystem
 */
void resfs_init(void);

/**
 * resfs_bind_domain - Expose a domain to userspace via resfs
 */
void resfs_bind_domain(struct resdomain *rd);

/**
 * resdomain_create - Create a new sub-domain
 */
struct resdomain *resdomain_create(struct resdomain *parent, const char *name);

/**
 * resdomain_destroy - Free a domain when refcount reaches 0
 */
void resdomain_destroy(struct resdomain *rd);

/**
 * resdomain_get - Increment refcount
 */
static inline void resdomain_get(struct resdomain *rd) {
    if (rd) atomic_inc(&rd->refcount);
}

/**
 * resdomain_put - Decrement refcount and destroy if 0
 */
void resdomain_put(struct resdomain *rd);

/**
 * resdomain_attach_task - Move a task to a different domain
 */
int resdomain_attach_task(struct resdomain *rd, struct task_struct *task);

/**
 * resdomain_task_init - Initialize task's ResDomain context during fork
 */
void resdomain_task_init(struct task_struct *p, struct task_struct *parent);
