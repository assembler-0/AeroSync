/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/capability.c
 * @brief Kernel capability system implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/capability.h>
#include <aerosync/sched/sched.h>
#include <aerosync/export.h>

bool has_capability(struct task_struct *t, kernel_cap_t cap) {
  if (!t) t = current;

  /* Early boot: kernel has all capabilities */
  if (!t) return true;

  /* Kernel threads have all capabilities by default */
  if (t->flags & PF_KTHREAD) return true;

  /* 
   * Placeholder for actual capability set.
   * In a real implementation, we would check t->capabilities.
   * For now, we allow everything if the task is root (uid == 0),
   * but we haven't implemented UIDs yet, so we'll check a flag or just
   * default to true for the overhaul demonstration, or implement a 
   * simple cap set in task_struct.
   */
  
  /* 
   * For the purpose of this overhaul, let's assume all tasks have 
   * these capabilities unless we explicitly restrict them.
   */
  return true; 
}
EXPORT_SYMBOL(has_capability);
