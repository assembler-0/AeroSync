#pragma once

#include <aerosync/compiler.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/atomic.h>

/**
 * @file include/aerosync/seqlock.h
 * @brief Sequence lock implementation
 * 
 * Seqlocks are fast read-mostly locks where readers never block writers.
 * Readers check a version counter to detect if a concurrent write occurred.
 */

typedef struct {
  unsigned int sequence;
} seqlock_t;

#define SEQLOCK_INIT { .sequence = 0 }

static inline void seqlock_init(seqlock_t *sl) {
  sl->sequence = 0;
}

/**
 * read_seqbegin - Start a seqlock read section
 * @sl: Pointer to seqlock
 * 
 * Returns the current sequence number to be passed to read_seqretry.
 */
static inline unsigned int read_seqbegin(const seqlock_t *sl) {
  unsigned int ret;
repeat:
  ret = READ_ONCE(sl->sequence);
  if (unlikely(ret & 1)) {
    cpu_relax();
    goto repeat;
  }
  smp_rmb();
  return ret;
}

/**
 * read_seqretry - End a seqlock read section and check for retries
 * @sl: Pointer to seqlock
 * @start: Sequence number from read_seqbegin
 * 
 * Returns true if the read was inconsistent and needs to be retried.
 */
static inline bool read_seqretry(const seqlock_t *sl, unsigned int start) {
  smp_rmb();
  return unlikely(READ_ONCE(sl->sequence) != start);
}

/**
 * write_seqlock - Acquire seqlock for writing
 * @sl: Pointer to seqlock
 * 
 * Note: Caller must handle mutual exclusion between writers (e.g. using a spinlock).
 * In many cases, seqlock is paired with a spinlock:
 * spin_lock(&lock); write_seqlock(&seqlock); ...
 */
static inline void write_seqlock(seqlock_t *sl) {
  sl->sequence++;
  smp_wmb();
}

/**
 * write_sequnlock - Release seqlock for writing
 */
static inline void write_sequnlock(seqlock_t *sl) {
  smp_wmb();
  sl->sequence++;
}

#define write_seqlock_irqsave(sl, flags) \
  do {                                   \
    flags = local_irq_save();            \
    write_seqlock(sl);                   \
  } while (0)

#define write_sequnlock_irqrestore(sl, flags) \
  do {                                        \
    write_sequnlock(sl);                      \
    local_irq_restore(flags);                 \
  } while (0)
