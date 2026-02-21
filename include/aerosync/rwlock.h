#pragma once

#include <aerosync/compiler.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/atomic.h>

/**
 * @file include/aerosync/rwlock.h
 * @brief Spin-based Read-Writer lock implementation
 * 
 * Allows multiple concurrent readers or a single exclusive writer.
 * Uses a 32-bit counter: 
 *   - MSB (bit 31) is the writer-locked bit.
 *   - Bits 0-30 are the reader count.
 */

typedef struct {
  atomic_t cnt;
} rwlock_t;

#define RWLOCK_INIT { .cnt = { .counter = 0 } }
#define RWLOCK_WRITE_BIT 0x80000000

static inline void rwlock_init(rwlock_t *lock) {
  atomic_set(&lock->cnt, 0);
}

/**
 * read_lock - Acquire rwlock for reading
 */
static inline void read_lock(rwlock_t *lock) {
  while (1) {
    int val = atomic_read(&lock->cnt);
    if (unlikely(val & RWLOCK_WRITE_BIT)) {
      cpu_relax();
      continue;
    }
    if (atomic_cmpxchg(&lock->cnt, val, val + 1) == val) {
      break;
    }
  }
}

/**
 * read_unlock - Release rwlock for reading
 */
static inline void read_unlock(rwlock_t *lock) {
  atomic_dec(&lock->cnt);
}

/**
 * write_lock - Acquire rwlock for writing
 */
static inline void write_lock(rwlock_t *lock) {
  while (atomic_cmpxchg(&lock->cnt, 0, RWLOCK_WRITE_BIT) != 0) {
    cpu_relax();
  }
}

/**
 * write_unlock - Release rwlock for writing
 */
static inline void write_unlock(rwlock_t *lock) {
  atomic_set(&lock->cnt, 0);
}

/**
 * read_trylock - Try to acquire rwlock for reading
 */
static inline int read_trylock(rwlock_t *lock) {
  int val = atomic_read(&lock->cnt);
  if (unlikely(val & RWLOCK_WRITE_BIT)) {
    return 0;
  }
  return atomic_cmpxchg(&lock->cnt, val, val + 1) == val;
}

#define write_lock_irqsave(lock, flags) \
  do {                                  \
    flags = local_irq_save();           \
    write_lock(lock);                   \
  } while (0)

#define write_unlock_irqrestore(lock, flags) \
  do {                                       \
    write_unlock(lock);                      \
    local_irq_restore(flags);                \
  } while (0)

#define read_lock_irqsave(lock, flags) \
  do {                                 \
    flags = local_irq_save();          \
    read_lock(lock);                   \
  } while (0)

#define read_unlock_irqrestore(lock, flags) \
  do {                                      \
    read_unlock(lock);                      \
    local_irq_restore(flags);               \
  } while (0)
