#pragma once

#include <linux/types.h>
#include <aerosync/preempt.h>

/*
 * Barriers and pointer accessors.
 */
#ifndef rcu_dereference
#define rcu_dereference(p) ({\
    typeof(p) _________p1 = smp_load_acquire(&(p)); \
    _________p1; \
})
#endif

#ifndef rcu_dereference_raw
#define rcu_dereference_raw(p) ({ \
    typeof(p) _________p1 = READ_ONCE(p); \
    _________p1; \
})
#endif

#ifndef rcu_assign_pointer
#define rcu_assign_pointer(p, v) do { \
    smp_store_release(&(p), (v)); \
} while (0)
#endif

#ifndef RCU_INIT_POINTER
#define RCU_INIT_POINTER(p, v)	do { (p) = (v); } while (0)
#endif

/*
 * RCU read-side critical sections.
 * For non-preemptible RCU, this just disables preemption.
 */
#ifndef rcu_read_lock
static inline void rcu_read_lock(void) {
    preempt_disable();
}
#endif

#ifndef rcu_read_unlock
static inline void rcu_read_unlock(void) {
    preempt_enable();
}
#endif

/*
 * Barriers and pointer accessors (more complex ones).
 */
#define rcu_dereference_check(p, c) ({ \
    unmet_cond_warn(!(c)) \
    rcu_dereference(p); \
})

#define rcu_dereference_protected(p, c) ({ \
    unmet_cond_warn(!(c)); \
    rcu_dereference_raw(p); \
})

#define rcu_dereference_raw_check(p) rcu_dereference_raw(p)

/*
 * API Prototypes
 */
struct rcu_head;
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));
void synchronize_rcu(void);
void rcu_barrier(void);

/* Check if in RCU read-side critical section (debug) */
#define rcu_read_lock_held() (preempt_count() > 0)

/* Internal init */
void rcu_check_callbacks(void);

