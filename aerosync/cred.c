// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/cred.c
 * @brief Credentials management implementation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/cred.h>
#include <aerosync/sched/sched.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <aerosync/export.h>

struct cred init_cred = {
  .usage = REFCOUNT_INIT(1),
  .uid = 0,
  .gid = 0,
  .suid = 0,
  .sgid = 0,
  .euid = 0,
  .egid = 0,
  .fsuid = 0,
  .fsgid = 0,
  .cap_inheritable = 0,
  .cap_permitted = ~0ULL,
  .cap_effective = ~0ULL,
  .cap_bset = ~0ULL,
  .cap_ambient = 0,
  .user_uuid = {{0}},
};
EXPORT_SYMBOL(init_cred);

struct cred *prepare_creds(void) {
  struct task_struct *task = current;
  struct cred *old = task->cred;
  struct cred *new;

  new = kmalloc(sizeof(struct cred));
  if (!new) return nullptr;

  if (old) {
    memcpy(new, old, sizeof(struct cred));
  } else {
    memcpy(new, &init_cred, sizeof(struct cred));
  }

  refcount_set(&new->usage, 1);
  return new;
}
EXPORT_SYMBOL(prepare_creds);

int commit_creds(struct cred *new) {
  struct task_struct *task = current;
  struct cred *old = task->cred;

  /* 
   * In a real kernel, we might need to check if we are allowed 
   * to change credentials here.
   */

  task->cred = new;
  if (old) put_cred(old);

  return 0;
}
EXPORT_SYMBOL(commit_creds);

void abort_creds(struct cred *new) {
  if (new) {
    if (refcount_read(&new->usage) != 1) {
      panic("abort_creds: credentials in use");
    }
    kfree(new);
  }
}
EXPORT_SYMBOL(abort_creds);

void put_cred(struct cred *cred) {
  if (cred && refcount_dec_and_test(&cred->usage)) {
    kfree(cred);
  }
}
EXPORT_SYMBOL(put_cred);
