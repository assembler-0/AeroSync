#pragma once

#include <aerosync/spinlock.h>
#include <limine/limine.h>
#include <aerosync/atomic.h>
#include <linux/list.h>

// Initialize SMP (BSP calls this)
void smp_init(void);

// Parse topology early (sets cpu_count)
void smp_parse_topology(void);

// Prepare the boot CPU (sets cpu_number per-cpu var)
void smp_prepare_boot_cpu(void);

// Get number of CPUs found
uint64_t smp_get_cpu_count(void);

uint64_t smp_get_id(void);

int smp_is_active();

/* --- Scalable SMP Cross-CPU Calls --- */

typedef void (*smp_call_func_t)(void *info);

#define CALL_FUNCTION_IPI_VECTOR 0xFC

struct smp_call_data {
    smp_call_func_t func;
    void *info;
    atomic_t finished;
};

/* Structure for a single cross-CPU call request */
struct call_single_data {
    struct list_head list;
    smp_call_func_t func;
    void *info;
    uint32_t flags; // CSD_FLAG_WAIT, etc.
};

#define CSD_FLAG_WAIT 0x01

/* Per-CPU queue of pending calls */
struct smp_call_queue {
    struct list_head list;
    spinlock_t lock;
};

/**
 * Execute a function on all other CPUs and wait for completion.
 */
void smp_call_function(smp_call_func_t func, void *info, bool wait);

/**
 * Execute a function on a set of CPUs.
 */
struct cpumask;
void smp_call_function_many(const struct cpumask *mask, smp_call_func_t func, void *info, bool wait);

/**
 * Execute a function on a specific CPU and wait for completion.
 */
void smp_call_function_single(int cpu, smp_call_func_t func, void *info, bool wait);

void smp_call_ipi_handler(void);
