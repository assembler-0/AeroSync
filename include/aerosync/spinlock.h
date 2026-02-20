#pragma once

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/atomic.h>
#ifndef CONFIG_DEBUG_SPINLOCK
#include <aerosync/errno.h>
#endif

#define DEADLOCK_TIMEOUT_CYCLES 100000000ULL
#define MAX_BACKOFF_CYCLES 1024

#ifdef CONFIG_TICKET_SPINLOCKS

typedef struct {
  union {
    uint32_t val;
    struct {
      uint16_t owner;
      uint16_t next;
    };
  };
#ifdef CONFIG_DEBUG_SPINLOCK
  int owner_cpu;
#endif
} spinlock_t;

#ifdef CONFIG_DEBUG_SPINLOCK
#define SPINLOCK_INIT { .val = 0, .owner_cpu = -1 }
#else
#define SPINLOCK_INIT { .val = 0 }
#endif

#else /* !CONFIG_TICKET_SPINLOCKS */

// A spinlock is a simple integer flag stored inline; pass its address to APIs
typedef struct {
  volatile int lock;
#ifdef CONFIG_DEBUG_SPINLOCK
  int owner_cpu;
#endif
} spinlock_t;

#ifdef CONFIG_DEBUG_SPINLOCK
#define SPINLOCK_INIT { .lock = 0, .owner_cpu = -1 }
#else
#define SPINLOCK_INIT { .lock = 0 }
#endif

#endif

#define SPINLOCK_LOCKED 1
#define SPINLOCK_UNLOCKED 0

#define DEFINE_SPINLOCK(name) spinlock_t name = SPINLOCK_INIT

// Initialize a spinlock
static inline void spinlock_init(spinlock_t *lock) {
  *lock = (spinlock_t)SPINLOCK_INIT;
}

#ifdef CONFIG_TICKET_SPINLOCKS

extern uint32_t smp_get_id(void);

static inline void spinlock_lock(spinlock_t *lock) {
  uint16_t ticket = __atomic_fetch_add(&lock->next, 1, __ATOMIC_RELAXED);
  while (READ_ONCE(lock->owner) != ticket) {
    cpu_relax();
  }
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
#ifdef CONFIG_DEBUG_SPINLOCK
  lock->owner_cpu = (int)smp_get_id();
#endif
}

static inline void spinlock_unlock(spinlock_t *lock) {
#ifdef CONFIG_DEBUG_SPINLOCK
  lock->owner_cpu = -1;
#endif
  __atomic_fetch_add(&lock->owner, 1, __ATOMIC_RELEASE);
}

static inline int spinlock_trylock(spinlock_t *lock) {
  uint32_t val = READ_ONCE(lock->val);
  uint16_t owner = (uint16_t)(val & 0xFFFF);
  uint16_t next = (uint16_t)(val >> 16);

  if (owner != next) return 0;

  uint32_t new_val = val + (1 << 16);
  if (cmpxchg(&lock->val, val, new_val) == val) {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#ifdef CONFIG_DEBUG_SPINLOCK
    lock->owner_cpu = (int)smp_get_id();
#endif
    return 1;
  }
  return 0;
}

static inline int spinlock_is_locked(spinlock_t *lock) {
  uint32_t val = READ_ONCE(lock->val);
  return (uint16_t)(val & 0xFFFF) != (uint16_t)(val >> 16);
}

static inline uint32_t spinlock_owner(spinlock_t *lock) {
  return READ_ONCE(lock->owner);
}

static inline uint32_t spinlock_next(spinlock_t *lock) {
  return READ_ONCE(lock->next);
}

#else /* !CONFIG_TICKET_SPINLOCKS */

// Advanced spinlock with exponential backoff
static inline void spinlock_lock(spinlock_t *lock) {
  uint32_t backoff = 1;

  while (__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
    // Exponential backoff to reduce contention
    for (uint32_t i = 0; i < backoff; i++) {
      cpu_relax();
    }
    // Cap backoff at 1024 iterations
    if (backoff < 1024) {
      backoff <<= 1;
    }
  }
#ifdef CONFIG_DEBUG_SPINLOCK
  lock->owner_cpu = (int)smp_get_id();
#endif
}

static inline int spinlock_trylock(spinlock_t *lock) {
  if (!__atomic_test_and_set(&lock->lock, __ATOMIC_ACQUIRE)) {
#ifdef CONFIG_DEBUG_SPINLOCK
    lock->owner_cpu = (int)smp_get_id();
#endif
    return 1;
  }
  return 0;
}

static inline int spinlock_is_locked(spinlock_t *lock) {
  return lock->lock;
}

static inline void spinlock_unlock(spinlock_t *lock) {
#ifdef CONFIG_DEBUG_SPINLOCK
  lock->owner_cpu = -1;
#endif
  __atomic_clear(&lock->lock, __ATOMIC_RELEASE);
}

#endif

static inline uint32_t spinlock_get_cpu(spinlock_t *lock) {
#if CONFIG_DEBUG_SPINLOCK && CONFIG_TICKET_SPINLOCKS
  return READ_ONCE(lock->owner_cpu);
#else
  return -ENODEV;
#endif
}

static inline irq_flags_t spinlock_lock_irqsave(spinlock_t *lock) {
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  spinlock_lock(lock);
  return flags;
}

static inline void spinlock_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags) {
  spinlock_unlock(lock);
  restore_irq_flags(flags);
}

#ifdef SPINLOCK_LINUX_COMPAT

# define __SPINLOCK_UNLOCKED SPINLOCK_UNLOCKED
# define __SPINLOCK_LOCKED SPINLOCK_LOCKED
# define __SPIN_LOCK_UNLOCKED(x) __SPINLOCK_UNLOCKED
# define __SPINLOCK_INIT(x) __SPINLOCK_UNLOCKED

static inline void spin_lock_irqsave(spinlock_t *lock, irq_flags_t *flags) {
  *flags = save_irq_flags();
  cpu_cli();
  spinlock_lock(lock);
}

static inline int spin_trylock(spinlock_t *lock) {
  return spinlock_trylock(lock);
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, irq_flags_t flags) {
  spinlock_unlock_irqrestore(lock, flags);
}

static inline void spin_lock(spinlock_t *lock) {
  spinlock_lock(lock);
}

static inline void spin_unlock(spinlock_t *lock) {
  spinlock_unlock(lock);
}

static inline void spin_lock_init(spinlock_t *lock) {
  spinlock_init(lock);
}

#endif /* SPINLOCK_LINUX_COMPAT */