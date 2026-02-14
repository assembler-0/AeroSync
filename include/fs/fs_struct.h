#pragma once

#include <aerosync/spinlock.h>
#include <aerosync/atomic.h>

struct dentry;

/**
 * struct fs_struct - Filesystem information for a process
 * 
 * This structure tracks the root and current working directory
 * for a task. Inspired by Linux.
 */
struct fs_struct {
    atomic_t count;
    spinlock_t lock;
    struct dentry *root;
    struct dentry *pwd;
};

struct fs_struct *copy_fs_struct(struct fs_struct *old_fs);
void free_fs_struct(struct fs_struct *fs);
