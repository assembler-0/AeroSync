#pragma once

#include <compiler.h>
#include <aerosync/types.h>
#include <arch/x86_64/percpu.h>

/**
 * SoftIRQ vectors (ordered by priority)
 */
enum {
    HI_SOFTIRQ = 0,
    TIMER_SOFTIRQ,
    NET_TX_SOFTIRQ,
    NET_RX_SOFTIRQ,
    BLOCK_SOFTIRQ,
    IRQ_POLL_SOFTIRQ,
    TASKLET_SOFTIRQ,
    SCHED_SOFTIRQ,
    HRTIMER_SOFTIRQ,
    RCU_SOFTIRQ,
    NR_SOFTIRQS
};

struct softirq_action {
    void (*action)(struct softirq_action *);
};

/**
 * raise_softirq - Raise a softirq on the local CPU
 */
void raise_softirq(unsigned int nr);

/**
 * open_softirq - Register a softirq handler
 */
void open_softirq(int nr, void (*action)(struct softirq_action *));

/**
 * invoke_softirq - Process pending softirqs on the local CPU
 */
void invoke_softirq(void);

/**
 * irq_enter - Called at the beginning of a hard interrupt
 */
void irq_enter(void);

/**
 * irq_exit - Called at the end of a hard interrupt
 */
void irq_exit(void);

/**
 * in_interrupt - Check if currently in hard or soft interrupt context
 */
bool in_interrupt(void);

/**
 * in_softirq - Check if currently in soft interrupt context
 */
bool in_softirq(void);

/**
 * softirq_init - Initialize the softirq system
 */
int __must_check softirq_init(void);

/**
 * softirq_init_ap - Initialize softirq on APs
 */
int __must_check softirq_init_ap(void);

/* Per-CPU softirq pending mask */
DECLARE_PER_CPU(uint32_t, softirq_pending);
