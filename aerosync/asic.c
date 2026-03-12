/* SPDX-License-Identifier: GPL-2.0-only */
/**
 * AeroSync Init Chain (ASIC)
 *
 * @file aerosync/asic.c
 * @brief Implementation of the AeroSync Init Chain runner.
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/asic.h>
#include <aerosync/panic.h>
#include <aerosync/errno.h>
#include <aerosync/classes.h>
#include <lib/printk.h>
#include <lib/log.h>

#ifdef CONFIG_ASIC_PROFILING
#include <aerosync/timer.h>
#endif

/* Symbols defined by the linker script */
extern const struct asic_descriptor __asic_start[];
extern const struct asic_descriptor __asic_end[];

/**
 * @brief Executes a single initcall and handles its result.
 */
static int asic_execute_one(const struct asic_descriptor *desc) {
  if (!desc || !desc->func)
    return -EINVAL;

#ifdef CONFIG_ASIC_DEBUG
  printk(KERN_DEBUG ASIC_CLASS "calling %s (stage 0x%x, prio %u)\n",
         desc->name, desc->stage, desc->priority);
#endif

#ifdef CONFIG_ASIC_PROFILING
  uint64_t start = get_time_ns();
#endif

  const int ret = desc->func();

#ifdef CONFIG_ASIC_PROFILING
  uint64_t duration = get_time_ns() - start;
  if (duration > 1000000) {
    /* Log if > 1ms */
    printk(KERN_DEBUG ASIC_CLASS "%s took %llu ns\n", desc->name, duration);
  }
#endif

  if (ret < 0) {
    printk(KERN_ERR ASIC_CLASS "initcall %s failed with %d\n", desc->name, ret);
    if (desc->flags & ASIC_FLAG_CRITICAL) {
      panic(ASIC_CLASS "Critical initcall %s failed (%d)", desc->name, ret);
    }
  }

  return ret;
}

int asic_run_stage(enum asic_stage stage) {
  int count = 0;
  const struct asic_descriptor *desc;

  for (desc = __asic_start; desc < __asic_end; desc++) {
    if (desc->stage == stage) {
      int ret = asic_execute_one(desc);
      if (ret == 0) {
        count++;
      }
    }
  }

  return count;
}

void asic_run_all(void) {
  const struct asic_descriptor *desc;

#ifdef CONFIG_ASIC_DEBUG
  printk(KERN_INFO ASIC_CLASS "Running all registered initcalls\n");
#endif

  for (desc = __asic_start; desc < __asic_end; desc++) {
    asic_execute_one(desc);
  }
}
