// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/cred.h
 * @brief Credentials management
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/capability.h>
#include <linux/refcount.h>
#include <linux/uuid.h>

/**
 * struct cred - Kernel credentials
 *
 * This structure holds the security credentials for a task.
 * It is shared between threads of the same process.
 */
struct cred {
  refcount_t usage;
  uid_t uid;    /* Real UID */
  gid_t gid;    /* Real GID */
  uid_t suid;   /* Saved UID */
  gid_t sgid;   /* Saved GID */
  uid_t euid;   /* Effective UID */
  gid_t egid;   /* Effective GID */
  uid_t fsuid;  /* Filesystem UID */
  gid_t fsgid;  /* Filesystem GID */

  kernel_cap_t cap_inheritable;
  kernel_cap_t cap_permitted;
  kernel_cap_t cap_effective;
  kernel_cap_t cap_bset;
  kernel_cap_t cap_ambient;

  /* UUID for identifying the specific session or user globally */
  uuid_t user_uuid;
};

struct task_struct;

/**
 * prepare_creds - Prepare a new set of credentials
 *
 * Returns a new set of credentials, usually copied from current.
 */
struct cred *prepare_creds(void);

/**
 * commit_creds - Commit a set of credentials to the current task
 */
int commit_creds(struct cred *new);

/**
 * abort_creds - Abort a set of credentials being prepared
 */
void abort_creds(struct cred *new);

/**
 * get_cred - Increment reference count on credentials
 */
static inline struct cred *get_cred(struct cred *cred) {
  if (cred) refcount_inc(&cred->usage);
  return cred;
}

/**
 * put_cred - Decrement reference count on credentials
 */
void put_cred(struct cred *cred);

extern struct cred init_cred;
