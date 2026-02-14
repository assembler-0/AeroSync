#pragma once

#include <aerosync/types.h>
#include <linux/list.h>
#include <aerosync/spinlock.h>

struct timer_list {
    struct list_head entry;
    uint64_t expires; // Absolute time in nanoseconds
    void (*function)(struct timer_list *);
    void *data;
    uint32_t cpu;
};

struct timespec;

void timer_init_subsystem(void);
void timer_setup(struct timer_list *timer, void (*function)(struct timer_list *), void *data);
void timer_add(struct timer_list *timer, uint64_t expires_ns);
void timer_del(struct timer_list *timer);
int timer_pending(const struct timer_list *timer);

// Wall-clock timekeeping
void timekeeping_init(uint64_t boot_timestamp_sec);
void ktime_get_real_ts64(struct timespec *ts);
uint64_t ktime_get_real_ns(void);

// Monotonic time since boot
uint64_t get_time_ns(void);

// Called from IRQ handler
void timer_handler(void);
