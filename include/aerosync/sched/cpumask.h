/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sched/cpumask.h
 * @brief CPU affinity mask implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 */

#pragma once

#include <arch/x86_64/smp.h>
#include <aerosync/types.h>
#include <arch/x86_64/cpu.h>

/* Number of 64-bit words needed to hold MAX_CPUS bits */
#define CPUMASK_BITS ((MAX_CPUS + 63) / 64)

/**
 * struct cpumask - CPU affinity bitmask
 * @bits: Array of 64-bit words holding the bitmask
 *
 * A cpumask represents a set of CPUs. It is used to specify
 * which CPUs a task is allowed to run on (CPU affinity).
 */
struct cpumask {
  uint64_t bits[CPUMASK_BITS];
};

typedef struct cpumask cpumask_t;

/* Static initializers */
#define CPU_MASK_NONE {.bits = {0}}
#define CPU_MASK_ALL {.bits = {[0 ... CPUMASK_BITS - 1] = ~0ULL}}
#define CPU_MASK_CPU0 {.bits = {1ULL}}

/**
 * DECLARE_CPUMASK - Declare a cpumask variable
 * @name: Variable name
 */
#define DECLARE_CPUMASK(name) struct cpumask name

/**
 * cpumask_set_cpu - Set a CPU in the mask
 * @cpu: CPU number to set
 * @mask: Target cpumask
 */
static inline void cpumask_set_cpu(int cpu, struct cpumask *mask) {
  if (cpu >= 0 && cpu < MAX_CPUS) {
    mask->bits[cpu / 64] |= (1ULL << (cpu % 64));
  }
}

/**
 * cpumask_clear_cpu - Clear a CPU from the mask
 * @cpu: CPU number to clear
 * @mask: Target cpumask
 */
static inline void cpumask_clear_cpu(int cpu, struct cpumask *mask) {
  if (cpu >= 0 && cpu < MAX_CPUS) {
    mask->bits[cpu / 64] &= ~(1ULL << (cpu % 64));
  }
}

/**
 * cpumask_test_cpu - Test if a CPU is set in the mask
 * @cpu: CPU number to test
 * @mask: cpumask to test
 *
 * Returns: true if CPU is set, false otherwise
 */
static inline bool cpumask_test_cpu(int cpu, const struct cpumask *mask) {
  if (cpu < 0 || cpu >= MAX_CPUS)
    return false;
  return (mask->bits[cpu / 64] & (1ULL << (cpu % 64))) != 0;
}

/**
 * cpumask_clear - Clear all CPUs from mask
 * @mask: Target cpumask
 */
static inline void cpumask_clear(struct cpumask *mask) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    mask->bits[i] = 0;
  }
}

/**
 * cpumask_setall - Set all CPUs in mask
 * @mask: Target cpumask
 */
static inline void cpumask_setall(struct cpumask *mask) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    mask->bits[i] = ~0ULL;
  }
}

/**
 * cpumask_empty - Check if mask is empty
 * @mask: cpumask to check
 *
 * Returns: true if no CPUs are set
 */
static inline bool cpumask_empty(const struct cpumask *mask) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    if (mask->bits[i] != 0)
      return false;
  }
  return true;
}

/**
 * cpumask_weight - Count number of CPUs set in mask
 * @mask: cpumask to count
 *
 * Returns: Number of set CPUs
 */
static inline int cpumask_weight(const struct cpumask *mask) {
  int count = 0;
  for (int i = 0; i < CPUMASK_BITS; i++) {
    count += __builtin_popcountll(mask->bits[i]);
  }
  return count;
}

/**
 * cpumask_first - Find first set CPU in mask
 * @mask: cpumask to search
 *
 * Returns: First set CPU, or MAX_CPUS if none set
 */
static inline int cpumask_first(const struct cpumask *mask) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    if (mask->bits[i] != 0) {
      return i * 64 + __builtin_ctzll(mask->bits[i]);
    }
  }
  return MAX_CPUS;
}

/**
 * cpumask_next - Find next set CPU after @cpu
 * @cpu: CPU to start from (not included in search)
 * @mask: cpumask to search
 *
 * Returns: Next set CPU, or MAX_CPUS if none found
 */
static inline int cpumask_next(int cpu, const struct cpumask *mask) {
  cpu++;
  while (cpu < MAX_CPUS) {
    if (cpumask_test_cpu(cpu, mask))
      return cpu;
    cpu++;
  }
  return MAX_CPUS;
}

/**
 * cpumask_copy - Copy a cpumask
 * @dst: Destination cpumask
 * @src: Source cpumask
 */
static inline void cpumask_copy(struct cpumask *dst,
                                const struct cpumask *src) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    dst->bits[i] = src->bits[i];
  }
}

/**
 * cpumask_and - AND two cpumasks
 * @dst: Destination cpumask (dst = src1 & src2)
 * @src1: First source
 * @src2: Second source
 *
 * Returns: true if result is non-empty
 */
static inline bool cpumask_and(struct cpumask *dst, const struct cpumask *src1,
                               const struct cpumask *src2) {
  bool result = false;
  for (int i = 0; i < CPUMASK_BITS; i++) {
    dst->bits[i] = src1->bits[i] & src2->bits[i];
    if (dst->bits[i])
      result = true;
  }
  return result;
}

/**
 * cpumask_or - OR two cpumasks
 * @dst: Destination cpumask (dst = src1 | src2)
 * @src1: First source
 * @src2: Second source
 */
static inline void cpumask_or(struct cpumask *dst, const struct cpumask *src1,
                              const struct cpumask *src2) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    dst->bits[i] = src1->bits[i] | src2->bits[i];
  }
}

/**
 * cpumask_equal - Check if two cpumasks are equal
 * @mask1: First cpumask
 * @mask2: Second cpumask
 *
 * Returns: true if masks are equal
 */
static inline bool cpumask_equal(const struct cpumask *mask1,
                                 const struct cpumask *mask2) {
  for (int i = 0; i < CPUMASK_BITS; i++) {
    if (mask1->bits[i] != mask2->bits[i])
      return false;
  }
  return true;
}

extern struct cpumask cpu_online_mask;

/**
 * for_each_cpu - Iterate over all CPUs in a mask
 * @cpu: Loop variable (int)
 * @mask: cpumask pointer
 */
#define for_each_cpu(cpu, mask)                                                \
  for ((cpu) = cpumask_first(mask); (cpu) < MAX_CPUS;                          \
       (cpu) = cpumask_next((cpu), (mask)))

/**
 * for_each_online_cpu - Iterate over all online CPUs
 * @cpu: Loop variable (int)
 *
 * Note: This uses smp_get_cpu_count() to determine online CPUs
 */
#define for_each_online_cpu(cpu)                                               \
  for ((cpu) = 0; (cpu) < (int)smp_get_cpu_count(); (cpu)++)

/**
 * for_each_possible_cpu - Iterate over all possible CPUs
 * @cpu: Loop variable (int)
 */
#define for_each_possible_cpu(cpu) for ((cpu) = 0; (cpu) < MAX_CPUS; (cpu)++)
