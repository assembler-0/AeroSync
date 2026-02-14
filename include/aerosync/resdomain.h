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
#include <aerosync/timer.h>

struct task_struct;
struct resdomain;
struct pseudo_node;

#define RD_MAX_SUBSYS 8

enum rd_subsys_id {
    RD_SUBSYS_CPU = 0,
    RD_SUBSYS_MEM,
    RD_SUBSYS_PID,
    RD_SUBSYS_IO,
    RD_SUBSYS_COUNT
};

/**
 * struct resdomain_subsys_state - Per-subsystem state for a domain
 */
struct resdomain_subsys_state {
    struct resdomain *rd;
};

/**
 * struct rd_subsys - Resource domain subsystem (controller) definition
 */
struct rd_subsys {
    const char *name;
    int id;
    
    struct resdomain_subsys_state *(*css_alloc)(struct resdomain *rd);
    void (*css_free)(struct resdomain *rd);
    
    int (*can_attach)(struct resdomain *rd, struct task_struct *task);
    void (*attach)(struct resdomain *rd, struct task_struct *task);

    /* Filesystem populator - called when a domain is bound to ResFS */
    void (*populate)(struct resdomain *rd, struct pseudo_node *dir);
};

struct resdomain {
    char name[64];
    atomic_t refcount;
    
    /* Hierarchy */
    struct resdomain *parent;
    struct list_head children;
    struct list_head sibling;
    
    /* Subsystem states */
    struct resdomain_subsys_state *subsys[RD_SUBSYS_COUNT];
    
    /* cgroup v2-style subtree control */
    uint32_t subtree_control; /* Controllers enabled for children */
    uint32_t child_subsys_mask; /* Controllers that CAN be enabled (from parent) */

    void *private_data; /* Mapping to filesystem node (e.g., pseudo_node) */

    /* Locking */
    spinlock_t lock;
};

extern struct resdomain root_resdomain;
extern struct rd_subsys *rd_subsys_list[RD_SUBSYS_COUNT];

/**
 * resdomain_init - Initialize the ResDomain subsystem
 */
void resdomain_init(void);

/**
 * resdomain_create - Create a new sub-domain
 */
struct resdomain *resdomain_create(struct resdomain *parent, const char *name);
bool resdomain_is_descendant(struct resdomain *parent, struct resdomain *child);

/**
 * resdomain_put - Decrement refcount and destroy if 0
 */
void resdomain_put(struct resdomain *rd);

/**
 * resdomain_get - Increment refcount
 */
static inline void resdomain_get(struct resdomain *rd) {
    if (rd) atomic_inc(&rd->refcount);
}

/**
 * resdomain_attach_task - Move a task to a different domain
 */
int resdomain_attach_task(struct resdomain *rd, struct task_struct *task);

/**
 * resdomain_task_init - Initialize task's ResDomain context during fork
 */
void resdomain_task_init(struct task_struct *p, struct task_struct *parent);

/**
 * resdomain_task_exit - Clean up task's ResDomain context during exit
 */
void resdomain_task_exit(struct task_struct *p);

/* --- Memory Controller API --- */
struct mem_rd_state {
    struct resdomain_subsys_state css;
    uint64_t max;
    uint64_t high;
    uint64_t low;
    atomic64_t usage;
};

int resdomain_charge_mem(struct resdomain *rd, uint64_t bytes, bool force);
void resdomain_uncharge_mem(struct resdomain *rd, uint64_t bytes);

/* --- PID Controller API --- */
struct pid_rd_state {
    struct resdomain_subsys_state css;
    int max;
    atomic_t count;
};

int resdomain_can_fork(struct resdomain *rd);
void resdomain_cancel_fork(struct resdomain *rd);

/* --- IO Controller API --- */
int resdomain_io_throttle(struct resdomain *rd, uint64_t bytes);

/* --- CPU Controller API --- */
struct cfs_bandwidth {
    spinlock_t lock;
    uint64_t period;  /* ns */
    uint64_t quota;   /* ns */
    uint64_t runtime; /* ns remaining in current period */
    uint64_t expires; /* ns */
    
    int timer_active;
    struct timer_list period_timer;
    struct list_head throttled_cfs_rq;
};

struct cpu_rd_state {
    struct resdomain_subsys_state css;
    struct sched_entity **se; /* Per-CPU entities for this domain */
    struct cfs_rq **cfs_rq;   /* Per-CPU runqueues for children of this domain */
    uint32_t weight;
    struct cfs_bandwidth bandwidth;
    atomic64_t usage;         /* Aggregate CPU time in ns */
};

/* --- Filesystem hooks --- */
void resfs_init(void);
void resfs_bind_domain(struct resdomain *rd);
