#pragma once

#include <arch/x86_64/tsc.h>
#include <aerosync/types.h>
#include <arch/x86_64/cpu.h>

#define DEADLOCK_TIMEOUT_CYCLES 100000000ULL
#define MAX_BACKOFF_CYCLES 1024
#define SPINLOCK_INIT 0
#define SPINLOCK_LOCKED 1
#define SPINLOCK_UNLOCKED 0

// A spinlock is a simple integer flag stored inline; pass its address to APIs
typedef volatile int spinlock_t;

// Initialize a spinlock
static inline void spinlock_init(spinlock_t *lock) {
  *lock = 0;
}

// Exponential backoff delay
static inline void backoff_delay(uint64_t cycles) {
  uint64_t start = rdtsc();
  while (rdtsc() - start < cycles) {
    cpu_relax();
  }
}

// Advanced spinlock with exponential backoff
static inline void spinlock_lock(volatile int *lock) {
  uint32_t backoff = 1;

  while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
    // Exponential backoff to reduce contention
    for (uint32_t i = 0; i < backoff; i++) {
      cpu_relax();
    }
    // Cap backoff at 1024 iterations
    if (backoff < 1024) {
      backoff <<= 1;
    }
  }
}

static inline int spinlock_trylock(volatile int *lock) {
  return !__atomic_test_and_set(lock, __ATOMIC_ACQUIRE);
}

static inline int spinlock_is_locked(volatile int *lock) {
  return *lock;
}

static inline void spinlock_unlock(volatile int *lock) {
  __atomic_clear(lock, __ATOMIC_RELEASE);
}

static inline irq_flags_t spinlock_lock_irqsave(volatile int *lock) {
  irq_flags_t flags = save_irq_flags();
  cpu_cli();
  spinlock_lock(lock); // Uses the advanced version above
  return flags;
}

static inline void spinlock_unlock_irqrestore(volatile int *lock, irq_flags_t flags) {
  __atomic_clear(lock, __ATOMIC_RELEASE);
  restore_irq_flags(flags);
}

#ifdef SPINLOCK_LINUX_COMPAT

# define __SPINLOCK_UNLOCKED SPINLOCK_UNLOCKED
# define __SPINLOCK_LOCKED SPINLOCK_LOCKED
# define __SPIN_LOCK_UNLOCKED(x) __SPINLOCK_UNLOCKED
# define __SPINLOCK_INIT(x) __SPINLOCK_UNLOCKED

static inline void spin_lock_irqsave(volatile int *lock, irq_flags_t *flags) {
  *flags = save_irq_flags();
  cpu_cli();
  spinlock_lock(lock);
}

static inline int spin_trylock(volatile int *lock) {
  return spinlock_trylock(lock);
}

static inline void spin_unlock_irqrestore(volatile int *lock, irq_flags_t flags) {
  spinlock_unlock_irqrestore(lock, flags);
}

static inline void spin_lock(volatile int *lock) {
  spinlock_lock(lock);
}

static inline void spin_unlock(volatile int *lock) {
  spinlock_unlock(lock);
}

#endif /* SPINLOCK_LINUX_COMPAT */