#include <aerosync/timer.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/percpu.h>
#include <arch/x86_64/tsc.h>
#include <fs/vfs.h>
#include <aerosync/panic.h>
#include <linux/container_of.h>

/* Wall-clock timekeeping state */
static struct {
  uint64_t boot_timestamp_ns;
  spinlock_t lock;
} timekeeper = {
  .boot_timestamp_ns = 0,
  .lock = SPINLOCK_INIT,
};

void timekeeping_init(uint64_t boot_timestamp_sec) {
  spinlock_lock(&timekeeper.lock);
  timekeeper.boot_timestamp_ns = boot_timestamp_sec * NSEC_PER_SEC;
  spinlock_unlock(&timekeeper.lock);
}

uint64_t ktime_get_real_ns(void) {
  return timekeeper.boot_timestamp_ns + get_time_ns();
}

void ktime_get_real_ts64(struct timespec *ts) {
  uint64_t ns = ktime_get_real_ns();
  ts->tv_sec = ns / NSEC_PER_SEC;
  ts->tv_nsec = ns % NSEC_PER_SEC;
}

struct timer_cpu_base {
  struct list_head active_timers;
  spinlock_t lock;
};

DEFINE_PER_CPU(struct timer_cpu_base, timer_bases);

void timer_init_subsystem(void) {
  for (int i = 0; i < MAX_CPUS; i++) {
    struct timer_cpu_base *base = per_cpu_ptr(timer_bases, i);
    INIT_LIST_HEAD(&base->active_timers);
    spinlock_init(&base->lock);
  }
}

static void __timer_reprogram(struct timer_cpu_base *base) {
  (void) base;
  // In periodic mode, we don't need to reprogram the hardware timer.
  // Software timers will be checked on every tick.
}

void timer_setup(struct timer_list *timer, void (*function)(struct timer_list *), void *data) {
  timer->function = function;
  timer->data = data;
  timer->cpu = smp_get_id();
  INIT_LIST_HEAD(&timer->entry);
}

void timer_add(struct timer_list *timer, uint64_t expires_ns) {
  timer->expires = expires_ns;
  int cpu = smp_get_id();
  timer->cpu = cpu;
  struct timer_cpu_base *base = this_cpu_ptr(timer_bases);

  irq_flags_t flags = spinlock_lock_irqsave(&base->lock);

  struct timer_list *pos;
  struct list_head *insert_after = &base->active_timers;

  list_for_each_entry(pos, &base->active_timers, entry) {
    if (pos->expires > expires_ns) {
      break;
    }
    insert_after = &pos->entry;
  }

  list_add_tail(&timer->entry, insert_after);

  // If we are the new head, reprogram
  if (base->active_timers.next == &timer->entry) {
    __timer_reprogram(base);
  }

  spinlock_unlock_irqrestore(&base->lock, flags);
}

void timer_del(struct timer_list *timer) {
  struct timer_cpu_base *base = per_cpu_ptr(timer_bases, timer->cpu);

  irq_flags_t flags = spinlock_lock_irqsave(&base->lock);
  if (!list_empty(&timer->entry)) {
    int was_head = (base->active_timers.next == &timer->entry);
    list_del_init(&timer->entry);
    if (was_head) {
      __timer_reprogram(base);
    }
  }
  spinlock_unlock_irqrestore(&base->lock, flags);
}

void __no_cfi timer_handler(void) {
  struct timer_cpu_base *base = this_cpu_ptr(timer_bases);
  uint64_t now = get_time_ns();

  // 1. Process expired timers
  irq_flags_t flags = spinlock_lock_irqsave(&base->lock);
  while (!list_empty(&base->active_timers)) {
    struct timer_list *timer = list_first_entry(&base->active_timers, struct timer_list, entry);
    if (timer->expires > now) {
      break;
    }

    list_del_init(&timer->entry);
    spinlock_unlock_irqrestore(&base->lock, flags);

    if (timer->function) {
      timer->function(timer);
    }

    flags = spinlock_lock_irqsave(&base->lock);
  }

  // 2. Reprogram for next timer
  __timer_reprogram(base);
  spinlock_unlock_irqrestore(&base->lock, flags);

  // 3. Scheduler tick
  scheduler_tick();
  check_preempt();
}
