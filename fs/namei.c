/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/namei.c
 * @brief Path lookup and inode creation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <aerosync/sched/sched.h>
#include <fs/fs_struct.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>

struct dentry *root_dentry = nullptr;

/**
 * struct mount - Represents an instance of a mounted filesystem
 */
static LIST_HEAD(mount_list);
static DEFINE_SPINLOCK(mount_lock);

static struct dentry *follow_mount(struct dentry *dentry) {
  struct mount *mnt;
  bool found;

  do {
    found = false;
    spinlock_lock(&mount_lock);
    list_for_each_entry(mnt, &mount_list, mnt_list) {
      if (mnt->mnt_mountpoint == dentry) {
        dentry = mnt->mnt_root;
        found = true;
        break;
      }
    }
    spinlock_unlock(&mount_lock);
  } while (found);

  return dentry;
}

int vfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  if (!dir->i_op || !dir->i_op->mkdir)
    return -EPERM;

  int ret = dir->i_op->mkdir(dir, dentry, mode);
  if (ret == 0) {
    list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_mkdir);

int vfs_mknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
  if (!dir->i_op || !dir->i_op->create)
    return -EPERM;

  /* For now, we use create for regular files and assume create can handle mode */
  int ret = dir->i_op->create(dir, dentry, mode);
  if (ret == 0) {
    list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_mknod);

static const char *get_next_component(const char *path, char *name) {
  while (*path == '/') path++;
  if (*path == '\0') return nullptr;

  int i = 0;
  while (*path != '/' && *path != '\0') {
    if (i < 255) name[i++] = *path;
    path++;
  }
  name[i] = '\0';
  return path;
}

struct dentry *vfs_path_lookup(const char *path, unsigned int flags) {
  (void) flags;
  if (!path) return nullptr;

  struct dentry *curr;

  if (path[0] == '/') {
    curr = dget(root_dentry);
  } else {
    if (current && current->fs && current->fs->pwd) {
      curr = dget(current->fs->pwd);
    } else {
      curr = dget(root_dentry);
    }
  }

  if (!curr) return nullptr;

  char component[256];
  const char *next = path;

  while ((next = get_next_component(next, component)) != nullptr) {
    if (strcmp(component, ".") == 0) {
      continue;
    }

    if (strcmp(component, "..") == 0) {
      if (curr->d_parent) {
        struct dentry *old = curr;
        curr = dget(curr->d_parent);
        dput(old);
      }
      continue;
    }

    /* Handle mount points */
    struct dentry *mounted = follow_mount(curr);
    if (mounted != curr) {
      dput(curr);
      curr = dget(mounted);
    }

    if (!curr->d_inode || !curr->d_inode->i_op || !curr->d_inode->i_op->lookup) {
      dput(curr);
      return nullptr;
    }

    struct qstr qname = {.name = (const unsigned char *) component, .len = (uint32_t) strlen(component)};

    /* Check if it's already in dentry cache subdirs */
    struct dentry *child;
    bool found = false;
    // Simplified cache lookup
    list_for_each_entry(child, &curr->d_subdirs, d_child) {
      if (strcmp((const char *) child->d_name.name, component) == 0) {
        struct dentry *old = curr;
        curr = dget(child);
        dput(old);
        found = true;
        break;
      }
    }

    if (found) continue;

    struct dentry *new_dentry = d_alloc_pseudo(curr->d_inode->i_sb, &qname);
    if (!new_dentry) {
      dput(curr);
      return nullptr;
    }

    new_dentry->d_parent = curr;
    struct dentry *result = curr->d_inode->i_op->lookup(curr->d_inode, new_dentry, 0);

    if (result == nullptr) {
      // Negative dentry or not found
      dput(new_dentry);
      dput(curr);
      return nullptr;
    }

    if (result != new_dentry) {
      dput(new_dentry);
      struct dentry *old = curr;
      curr = dget(result);
      dput(old);
    } else {
      /* Link into parent's subdirs */
      list_add_tail(&new_dentry->d_child, &curr->d_subdirs);
      struct dentry *old = curr;
      curr = dget(new_dentry);
      dput(old);
      dput(new_dentry);
    }
  }

  struct dentry *final = follow_mount(curr);
  if (final != curr) {
    dget(final);
    dput(curr);
    return final;
  }

  return curr;
}

struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name) {
  struct dentry *dentry = kzalloc(sizeof(struct dentry));
  if (!dentry) return nullptr;

  dentry->d_name.name = (unsigned char *) kstrdup((const char *) name->name);
  dentry->d_name.len = name->len;
  spinlock_init(&dentry->d_lock);
  atomic_set(&dentry->d_count, 1);
  INIT_LIST_HEAD(&dentry->d_subdirs);
  INIT_LIST_HEAD(&dentry->d_child);

  return dentry;
}

static void split_path(const char *path, char *parent_path, char *name) {
  const char *last_slash = strrchr(path, '/');
  if (!last_slash) {
    strcpy(parent_path, ".");
    strcpy(name, path);
  } else if (last_slash == path) {
    strcpy(parent_path, "/");
    strcpy(name, path + 1);
  } else {
    size_t len = last_slash - path;
    strncpy(parent_path, path, len);
    parent_path[len] = '\0';
    strcpy(name, last_slash + 1);
  }
}

int do_mkdir(const char *path, vfs_mode_t mode) {
  char parent_path[1024];
  char name[256];
  split_path(path, parent_path, name);

  struct dentry *parent = vfs_path_lookup(parent_path, 0);
  if (!parent) return -ENOENT;

  struct qstr qname = {.name = (const unsigned char *) name, .len = (uint32_t) strlen(name)};
  struct dentry *dentry = d_alloc_pseudo(parent->d_inode->i_sb, &qname);
  if (!dentry) {
    dput(parent);
    return -ENOMEM;
  }

  dentry->d_parent = parent;
  int ret = vfs_mkdir(parent->d_inode, dentry, mode);
  if (ret != 0) {
    dput(dentry);
  }

  dput(parent);
  return ret;
}

EXPORT_SYMBOL(do_mkdir);

int do_mknod(const char *path, vfs_mode_t mode, dev_t dev) {
  char parent_path[1024];
  char name[256];
  split_path(path, parent_path, name);

  struct dentry *parent = vfs_path_lookup(parent_path, 0);
  if (!parent) return -ENOENT;

  struct qstr qname = {.name = (const unsigned char *) name, .len = (uint32_t) strlen(name)};
  struct dentry *dentry = d_alloc_pseudo(parent->d_inode->i_sb, &qname);
  if (!dentry) {
    dput(parent);
    return -ENOMEM;
  }

  dentry->d_parent = parent;
  int ret = vfs_mknod(parent->d_inode, dentry, mode, dev);
  if (ret != 0) {
    dput(dentry);
  }

  dput(parent);
  return ret;
}

EXPORT_SYMBOL(do_mknod);
