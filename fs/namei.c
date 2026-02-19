/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/namei.c
 * @brief Path lookup and inode creation
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/classes.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/printk.h>
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

int vfs_create(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  if (!dir->i_op || !dir->i_op->create)
    return -EPERM;

  int ret = dir->i_op->create(dir, dentry, mode);
  if (ret == 0) {
    if (list_empty(&dentry->d_child))
        list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
    vfs_notify_change(dentry, VFS_EVENT_CREATE);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_create);

int vfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  if (!dir->i_op || !dir->i_op->mkdir)
    return -EPERM;

  int ret = dir->i_op->mkdir(dir, dentry, mode);
  if (ret == 0) {
    if (list_empty(&dentry->d_child))
        list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
    vfs_notify_change(dentry, VFS_EVENT_CREATE);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_mkdir);

int vfs_mknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
  if (!dir->i_op || !dir->i_op->create)
    return -EPERM;

  int ret = dir->i_op->create(dir, dentry, mode);
  if (ret == 0 && list_empty(&dentry->d_child)) {
    list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_mknod);

int vfs_unlink(struct inode *dir, struct dentry *dentry) {
  if (!dir->i_op || !dir->i_op->unlink)
    return -EPERM;

  int ret = dir->i_op->unlink(dir, dentry);
  if (ret == 0) {
    vfs_notify_change(dentry, VFS_EVENT_DELETE);
    if (!list_empty(&dentry->d_child))
      list_del_init(&dentry->d_child);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_unlink);

int vfs_rmdir(struct inode *dir, struct dentry *dentry) {
  if (!dir->i_op || !dir->i_op->rmdir)
    return -EPERM;

  int ret = dir->i_op->rmdir(dir, dentry);
  if (ret == 0) {
    vfs_notify_change(dentry, VFS_EVENT_DELETE);
    if (!list_empty(&dentry->d_child))
      list_del_init(&dentry->d_child);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_rmdir);

int vfs_rename(struct inode *old_dir, struct dentry *old_dentry,
               struct inode *new_dir, struct dentry *new_dentry) {
  if (!old_dir->i_op || !old_dir->i_op->rename)
    return -EPERM;

  int ret = old_dir->i_op->rename(old_dir, old_dentry, new_dir, new_dentry);
  if (ret == 0) {
    vfs_notify_change(old_dentry, VFS_EVENT_DELETE);
    vfs_notify_change(new_dentry, VFS_EVENT_CREATE);

    /* Update parent and subdirs list */
    if (!list_empty(&old_dentry->d_child))
      list_del_init(&old_dentry->d_child);
    
    old_dentry->d_parent = new_dentry->d_parent;
    list_add_tail(&old_dentry->d_child, &old_dentry->d_parent->d_subdirs);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_rename);

int vfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname) {
  if (!dir->i_op || !dir->i_op->symlink)
    return -EPERM;

  int ret = dir->i_op->symlink(dir, dentry, oldname);
  if (ret == 0) {
    if (list_empty(&dentry->d_child))
        list_add_tail(&dentry->d_child, &dentry->d_parent->d_subdirs);
    vfs_notify_change(dentry, VFS_EVENT_CREATE);
  }
  return ret;
}

EXPORT_SYMBOL(vfs_symlink);

int vfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz) {
  if (!dentry->d_inode || !dentry->d_inode->i_op || !dentry->d_inode->i_op->readlink)
    return -EINVAL;
  return dentry->d_inode->i_op->readlink(dentry, buf, bufsiz);
}

EXPORT_SYMBOL(vfs_readlink);

struct dentry *vfs_path_lookup(const char *path, unsigned int flags);
static struct dentry *link_path_walk(const char *path, unsigned int flags, int *depth);

static int follow_link(struct dentry **dentry_ptr, int *depth) {
  struct dentry *dentry = *dentry_ptr;
  if (!dentry->d_inode || !S_ISLNK(dentry->d_inode->i_mode))
    return 0;

  if (++(*depth) > 8) {
    return -ELOOP;
  }

  void *cookie = nullptr;
  const char *link = nullptr;

  if (dentry->d_inode->i_op && dentry->d_inode->i_op->follow_link) {
    link = dentry->d_inode->i_op->follow_link(dentry, &cookie);
  } else {
    /* Fallback: try readlink if follow_link is not implemented */
    char *link_buf = kmalloc(4096);
    if (!link_buf) return -ENOMEM;
    int len = vfs_readlink(dentry, link_buf, 4096);
    if (len < 0) {
      kfree(link_buf);
      return len;
    }
    link_buf[len] = '\0';
    link = link_buf;
    cookie = link_buf;
  }

  if (!link) return -ENOENT;

  struct dentry *new_dentry = link_path_walk(link, 0, depth);
  
  if (dentry->d_inode->i_op && dentry->d_inode->i_op->put_link) {
    dentry->d_inode->i_op->put_link(dentry, cookie);
  } else if (cookie && !dentry->d_inode->i_op->follow_link) {
    /* Falling back from our manual readlink */
    kfree(cookie);
  }

  if (!new_dentry) return -ENOENT;

  dput(dentry);
  *dentry_ptr = new_dentry;
  return 0;
}
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
  int depth = 0;
  return link_path_walk(path, flags, &depth);
}

EXPORT_SYMBOL(vfs_path_lookup);

static struct dentry *link_path_walk(const char *path, unsigned int flags, int *depth) {
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
    if (flags & LOOKUP_PARENT) {
      /* Peek at next component */
      const char *peek = next;
      char next_comp[256];
      if (get_next_component(peek, next_comp) == nullptr) {
        /* This was the last component, stop here and return parent (curr) */
        return curr;
      }
    }

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
    list_for_each_entry(child, &curr->d_subdirs, d_child) {
      if (strcmp((const char *) child->d_name.name, component) == 0) {
        struct dentry *old = curr;
        curr = dget(child);
        dput(old);
        found = true;
        break;
      }
    }

    if (!found) {
      struct dentry *new_dentry = d_alloc_pseudo(curr->d_inode->i_sb, &qname);
      if (!new_dentry) {
        dput(curr);
        return nullptr;
      }

      new_dentry->d_parent = curr;
      struct dentry *result = curr->d_inode->i_op->lookup(curr->d_inode, new_dentry, 0);

      if (result == nullptr) {
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
        list_add_tail(&new_dentry->d_child, &curr->d_subdirs);
        struct dentry *old = curr;
        curr = dget(new_dentry);
        dput(old);
        dput(new_dentry);
      }
    }

    /* If it's a symlink, follow it unless it's the last component and !LOOKUP_FOLLOW */
    if (curr->d_inode && S_ISLNK(curr->d_inode->i_mode)) {
      const char *peek = next;
      char next_comp[256];
      bool last = (get_next_component(peek, next_comp) == nullptr);
      
      if (!last || (flags & LOOKUP_FOLLOW)) {
        if (follow_link(&curr, depth) < 0) {
          dput(curr);
          return nullptr;
        }
      }
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
  INIT_LIST_HEAD(&dentry->i_list);
  INIT_LIST_HEAD(&dentry->d_subscribers);

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

int do_unlink(const char *path) {
  char parent_path[1024];
  char name[256];
  split_path(path, parent_path, name);

  struct dentry *parent = vfs_path_lookup(parent_path, 0);
  if (!parent) return -ENOENT;

  struct dentry *dentry = vfs_path_lookup(path, 0);
  if (!dentry) {
    dput(parent);
    return -ENOENT;
  }

  int ret = vfs_unlink(parent->d_inode, dentry);
  dput(dentry);
  dput(parent);
  return ret;
}

EXPORT_SYMBOL(do_unlink);

int do_rmdir(const char *path) {
  char parent_path[1024];
  char name[256];
  split_path(path, parent_path, name);

  struct dentry *parent = vfs_path_lookup(parent_path, 0);
  if (!parent) return -ENOENT;

  struct dentry *dentry = vfs_path_lookup(path, 0);
  if (!dentry) {
    dput(parent);
    return -ENOENT;
  }

  int ret = vfs_rmdir(parent->d_inode, dentry);
  dput(dentry);
  dput(parent);
  return ret;
}

EXPORT_SYMBOL(do_rmdir);

int do_rename(const char *oldpath, const char *newpath) {
  char *old_parent_path = nullptr, *new_parent_path = nullptr;
  char *old_name = nullptr, *new_name = nullptr;
  struct dentry *old_parent = nullptr, *new_parent = nullptr;
  struct dentry *old_dentry = nullptr, *new_dentry = nullptr;
  int ret = -ENOMEM;

  if (strcmp(oldpath, newpath) == 0) return 0;

  old_parent_path = kmalloc(1024);
  new_parent_path = kmalloc(1024);
  old_name = kmalloc(256);
  new_name = kmalloc(256);

  if (!old_parent_path || !new_parent_path || !old_name || !new_name)
    goto out;

  split_path(oldpath, old_parent_path, old_name);
  split_path(newpath, new_parent_path, new_name);

  old_parent = vfs_path_lookup(old_parent_path, 0);
  if (!old_parent) {
    ret = -ENOENT;
    goto out;
  }

  new_parent = vfs_path_lookup(new_parent_path, 0);
  if (!new_parent) {
    ret = -ENOENT;
    goto out;
  }

  old_dentry = vfs_path_lookup(oldpath, 0);
  if (!old_dentry) {
    ret = -ENOENT;
    goto out;
  }

  new_dentry = vfs_path_lookup(newpath, 0);
  if (new_dentry) {
#ifdef CONFIG_VFS_RENAME_OVERWRITE
    /* Unlink or rmdir the existing destination if it's the same type */
    if (new_dentry->d_inode && old_dentry->d_inode) {
      if (S_ISDIR(old_dentry->d_inode->i_mode) && S_ISDIR(new_dentry->d_inode->i_mode)) {
        vfs_rmdir(new_parent->d_inode, new_dentry);
      } else if (!S_ISDIR(old_dentry->d_inode->i_mode) && !S_ISDIR(new_dentry->d_inode->i_mode)) {
        vfs_unlink(new_parent->d_inode, new_dentry);
      } else {
        /* Mixed types - usually error */
        ret = -EISDIR; // Or -ENOTDIR
        goto out;
      }
    }
    dput(new_dentry);
    new_dentry = nullptr;
#else
    /* Fail if destination exists */
    ret = -EEXIST;
    goto out;
#endif
  }

  struct qstr qname = {.name = (const unsigned char *) new_name, .len = (uint32_t) strlen(new_name)};
  new_dentry = d_alloc_pseudo(new_parent->d_inode->i_sb, &qname);
  if (!new_dentry) {
    ret = -ENOMEM;
    goto out;
  }
  new_dentry->d_parent = new_parent;

  ret = vfs_rename(old_parent->d_inode, old_dentry, new_parent->d_inode, new_dentry);

out:
  if (new_dentry) dput(new_dentry);
  if (old_dentry) dput(old_dentry);
  if (old_parent) dput(old_parent);
  if (new_parent) dput(new_parent);
  kfree(old_parent_path);
  kfree(new_parent_path);
  kfree(old_name);
  kfree(new_name);
  return ret;
}

EXPORT_SYMBOL(do_rename);

int do_symlink(const char *oldpath, const char *newpath) {
  char parent_path[1024];
  char name[256];
  split_path(newpath, parent_path, name);

  struct dentry *parent = vfs_path_lookup(parent_path, 0);
  if (!parent) return -ENOENT;

  struct qstr qname = {.name = (const unsigned char *) name, .len = (uint32_t) strlen(name)};
  struct dentry *dentry = d_alloc_pseudo(parent->d_inode->i_sb, &qname);
  if (!dentry) {
    dput(parent);
    return -ENOMEM;
  }

  dentry->d_parent = parent;
  int ret = vfs_symlink(parent->d_inode, dentry, oldpath);
  if (ret != 0) {
    dput(dentry);
  }

  dput(parent);
  return ret;
}

EXPORT_SYMBOL(do_symlink);

ssize_t do_readlink(const char *path, char *buf, size_t bufsiz) {
  struct dentry *dentry = vfs_path_lookup(path, 0); // Don't follow symlink itself!
  if (!dentry) return -ENOENT;

  ssize_t ret = vfs_readlink(dentry, buf, bufsiz);
  dput(dentry);
  return ret;
}

EXPORT_SYMBOL(do_readlink);
