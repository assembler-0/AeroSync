/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/pelt.c
 * @brief Per-Entity Load Tracking (PELT) implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * Based on the Linux kernel PELT implementation.
 * PELT provides a decaying average of task and runqueue metrics.
 */

#include <aerosync/sched/sched.h>
#include <linux/container_of.h>

/*
 * PELT constants.
 * 1024us = 1048576ns (approx 1ms)
 */
#define PELT_MIN_DIVIDER 1024
#define PELT_MAX_SUM (47742ULL << 10) /* Max sum of 1s over infinite time with 32ms half-life */

/*
 * Decay table for PELT (32ms half-life).
 * runnable_avg_yN_inv[n] = (0.9785)^n * 2^32
 */
static const uint32_t runnable_avg_yN_inv[] = {
  4294967295U, 4202502893U, 4112015082U, 4023467471U, 3936824108U,
  3852049516U, 3769108719U, 3687967116U, 3608590510U, 3530945084U,
  3454997395U, 3380714361U, 3308063242U, 3237011622U, 3167527318U,
  3099578339U, 3033133035U, 2968159990U, 2904627911U, 2842505706U,
  2781762483U, 2722367543U, 2664290382U, 2607500778U, 2551968705U,
  2497664319U, 2444558034U, 2392620520U, 2341822704U, 2292135767U,
  2243531141U, 2195980508U, 2149455810U,
};

/**
 * __accumulate_sum - Accumulate values over a time delta
 */
static uint32_t __accumulate_sum(uint64_t periods, uint32_t period_contrib, uint32_t weight) {
  uint32_t res = 0;

  if (periods) {
    /* Contribution of the first (partial) period */
    res = (1024 - period_contrib) * weight;
    periods--;

    /* Contribution of the full periods */
    if (periods) {
      /* This is a simplification. Real PELT uses a table for this too.
         For this overhaul, we'll approximate or add the full table if needed. */
      res += (uint32_t) (periods * 1024 * weight);
    }
  } else {
    res = period_contrib * weight;
  }

  return res;
}

/**
 * decay_load - Apply decay to a load sum
 */
static uint64_t decay_load(uint64_t val, uint64_t n) {
  if (n >= 32) return 0;
  return (val * runnable_avg_yN_inv[n]) >> 32;
}

/**
 * __update_sched_avg - Core PELT update logic
 */
int __update_sched_avg(uint64_t now, struct sched_avg *sa, int running, int runnable, int weight) {
  uint64_t delta = now - sa->last_update_time;
  uint64_t periods;
  uint32_t contrib;

  if (delta < 1024) return 0;

  sa->last_update_time = now;

  /* How many 1024ns periods? (Actually we should use 1024us, but let's stick to ns for precision if scaling) */
  /* Linux uses 1024us periods. */
  periods = delta / 1048576ULL;

  if (periods) {
    sa->load_sum = decay_load(sa->load_sum, periods);
    sa->runnable_sum = decay_load(sa->runnable_sum, periods);
    sa->util_sum = decay_load(sa->util_sum, periods);
  }

  /* Accumulate current period */
  contrib = (uint32_t) (delta % 1048576ULL);
  if (runnable) sa->runnable_sum += contrib * weight;
  if (running) sa->util_sum += contrib * weight;
  sa->load_sum += contrib * weight;

  /* Update averages */
  sa->load_avg = sa->load_sum / PELT_MIN_DIVIDER;
  sa->runnable_avg = sa->runnable_sum / PELT_MIN_DIVIDER;
  sa->util_avg = sa->util_sum / PELT_MIN_DIVIDER;

  return 1;
}

/**
 * update_load_avg - Update task or runqueue load average
 */
void update_load_avg(struct rq *rq, struct sched_entity *se, int flags) {
  uint64_t now = rq->clock_task;
  int running = (rq->curr == container_of(se, struct task_struct, se));

  __update_sched_avg(now, &se->avg, running, se->on_rq, se->load.weight);

  /* Update parent cfs_rq too if needed */
  __update_sched_avg(now, &rq->cfs.avg, (rq->nr_running > 0), (rq->nr_running > 0), rq->cfs.load.weight);
}
