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
#include <aerosync/cred.h>
#include <aerosync/export.h>

bool has_capability(struct task_struct *t, kernel_cap_t cap) {
  if (!t) t = current;

  /* Early boot: kernel has all capabilities */
  if (!t) return true;

  /* Kernel threads have all capabilities by default */
  if (t->flags & PF_KTHREAD) return true;

  if (!t->cred) return false;

  /* Root (UID 0) has all capabilities by default */
  if (t->cred->euid == 0) return true;

  /* Check the effective capability set */
  return (t->cred->cap_effective & cap) != 0;
}
EXPORT_SYMBOL(has_capability);
