/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/fs_struct.c
 * @brief Process filesystem context management
 * @copyright (C) 2026 assembler-0
 */

#include <fs/fs_struct.h>
#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>

struct fs_struct *copy_fs_struct(struct fs_struct *old_fs) {
  struct fs_struct *new_fs = kzalloc(sizeof(struct fs_struct));
  if (!new_fs) return nullptr;

  atomic_set(&new_fs->count, 1);
  spinlock_init(&new_fs->lock);

  if (old_fs) {
    spinlock_lock(&old_fs->lock);
    new_fs->root = old_fs->root;
    new_fs->pwd = old_fs->pwd;
    // In a full VFS, we would increment refcounts on dentries here
    dget(new_fs->root);
    dget(new_fs->pwd);
    spinlock_unlock(&old_fs->lock);
  }

  return new_fs;
}

void free_fs_struct(struct fs_struct *fs) {
  if (!fs) return;

  if (atomic_dec_and_test(&fs->count)) {
    dput(fs->root);
    dput(fs->pwd);
    kfree(fs);
  }
}
