/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/vfs.c
 * @brief Virtual File System core implementation
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <string.h>
#include <fs/vfs.h>
#include <fs/file.h>
#include <include/linux/list.h>
#include <aerosync/types.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/mutex.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <mm/slub.h>
#include <aerosync/errno.h>
#include <aerosync/resdomain.h>
#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/block.h>
#include <fs/devtmpfs.h>
#include <fs/sysfs.h>
#include <fs/procfs.h>
#include <fs/fs_struct.h>
#include <fs/initramfs.h>
#include <aerosync/timer.h>
#include <arch/x86_64/requests.h>
#include <mm/vm_object.h>
#include <lib/uaccess.h>

// Global lists for VFS objects
LIST_HEAD(super_blocks); // List of all mounted superblocks
LIST_HEAD(inodes); // List of all active inodes
LIST_HEAD(dentries); // List of all active dentries (dentry cache)

// Mutexes to protect global lists
static struct mutex sb_mutex;
static struct mutex inode_mutex;
static struct mutex dentry_mutex;

#ifndef INITRD_NAME_MAX_SIZE
#define INITRD_NAME_MAX_SIZE 128
#endif
static char initramfs_path[INITRD_NAME_MAX_SIZE];

LIST_HEAD(mount_list);
static struct mutex mount_mutex;

LIST_HEAD(file_systems); // List of all registered file system types
static struct mutex fs_type_mutex;

extern struct dentry *root_dentry;
extern struct file_system_type tmpfs_type;

int vfs_init(void) {
  printk(VFS_CLASS "Initializing Virtual File System...\n");

  extern void files_init(void);
  files_init();

  // Initialize global lists
  INIT_LIST_HEAD(&super_blocks);
  INIT_LIST_HEAD(&inodes);
  INIT_LIST_HEAD(&dentries);
  INIT_LIST_HEAD(&file_systems);
  INIT_LIST_HEAD(&mount_list);

  // Initialize mutexes
  mutex_init(&sb_mutex);
  mutex_init(&inode_mutex);
  mutex_init(&dentry_mutex);
  mutex_init(&mount_mutex);
  mutex_init(&fs_type_mutex);

  extern void tmpfs_init(void);
  tmpfs_init();

#ifdef CONFIG_RESFS
  resfs_init();
#endif

#ifdef CONFIG_SYSFS
  sysfs_init();
#endif

#ifdef CONFIG_PROCFS
  procfs_init();
#endif

  // Mount tmpfs as the base rootfs
  int mount_ret = vfs_mount(nullptr, "/", "tmpfs", 0, nullptr);
  if (mount_ret < 0) {
    return mount_ret;
  }

  /* Initialize current task's filesystem context */
  if (current) {
    current->fs = copy_fs_struct(nullptr);
    if (current->fs) {
      current->fs->root = dget(root_dentry);
      current->fs->pwd = dget(root_dentry);
    }
  }

  /* Unpack initramfs if enabled and found */
  cmdline_find_option(
    current_cmdline,
    "initrd",
    initramfs_path,
    sizeof(initramfs_path) /* INITRD_NAME_MAX_SIZE */
  );

  initramfs_init(initramfs_path);

#ifdef CONFIG_DEVTMPFS
  devtmpfs_init();

#ifdef CONFIG_DEVTMPFS_MOUNT
  vfs_mount(nullptr, STRINGIFY(CONFIG_DEVTMPFS_MOUNT_PATH), "devtmpfs", 0, nullptr);
#endif
#endif

#ifdef CONFIG_SYSFS
#ifdef CONFIG_SYSFS_MOUNT
  vfs_mount(nullptr, STRINGIFY(CONFIG_SYSFS_MOUNT_PATH), "sysfs", 0, nullptr);
#endif
#endif

#ifdef CONFIG_PROCFS
#ifdef CONFIG_PROCFS_MOUNT
  vfs_mount(nullptr, STRINGIFY(CONFIG_PROCFS_MOUNT_PATH), "proc", 0, nullptr);
#endif
#endif

#ifdef CONFIG_RESFS
#ifdef CONFIG_RESFS_MOUNT
  vfs_mount(nullptr, STRINGIFY(CONFIG_RESFS_MOUNT_PATH), "resfs", 0, nullptr);
#endif
#endif

  printk(VFS_CLASS "VFS initialization complete.\n");
  return 0;
}

EXPORT_SYMBOL(vfs_init);

int vfs_mount(const char *dev_name, const char *dir_name, const char *type, unsigned long flags, void *data) {
  struct file_system_type *fs;
  int ret = -ENODEV;

  mutex_lock(&fs_type_mutex);
  list_for_each_entry(fs, &file_systems, fs_list) {
    if (strcmp(fs->name, type) == 0) {
      /* Find mountpoint dentry */
      struct dentry *mountpoint = nullptr;
      if (root_dentry) {
        mountpoint = vfs_path_lookup(dir_name, 0);
        if (!mountpoint) {
          /* Auto-create parent directories if they don't exist */
          if (strcmp(dir_name, "/") != 0) {
            char *p_copy = kstrdup(dir_name);
            if (p_copy) {
              char *slash = strrchr(p_copy, '/');
              if (slash && slash != p_copy) {
                *slash = '\0';
                do_mkdir(p_copy, 0755);
              }
              kfree(p_copy);
            }
            mountpoint = vfs_path_lookup(dir_name, 0);
          }

          if (!mountpoint) {
            mutex_unlock(&fs_type_mutex);
            return -ENOENT;
          }
        }
      } else if (strcmp(dir_name, "/") != 0) {
        /* Cannot mount anywhere else if root is not yet mounted */
        mutex_unlock(&fs_type_mutex);
        return -ENOENT;
      }

      ret = fs->mount(fs, dev_name, dir_name, flags, data);
      if (ret == 0) {
        /* Record the mount */
        struct mount *mnt = kzalloc(sizeof(struct mount));
        struct super_block *sb = list_last_entry(&super_blocks, struct super_block, sb_list);

        mnt->mnt_sb = sb;
        mnt->mnt_root = dget(sb->s_root);
        mnt->mnt_mountpoint = mountpoint; // lookup returned a ref

        if (strcmp(dir_name, "/") == 0 && !root_dentry) {
          root_dentry = dget(sb->s_root);
        }

        /* Ensure the mount point points to the new root if it was a root mount */
        if (mountpoint == nullptr && strcmp(dir_name, "/") == 0) {
          mnt->mnt_mountpoint = dget(sb->s_root);
        }

        mutex_lock(&mount_mutex);
        list_add_tail(&mnt->mnt_list, &mount_list);
        mutex_unlock(&mount_mutex);
      } else {
        if (mountpoint) dput(mountpoint);
      }
      break;
    }
  }
  mutex_unlock(&fs_type_mutex);

  return ret;
}

EXPORT_SYMBOL(vfs_mount);

int sys_chdir(const char *path_user) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  struct dentry *dentry = vfs_path_lookup(path, 0);
  kfree(path);

  if (!dentry || !dentry->d_inode || !S_ISDIR(dentry->d_inode->i_mode)) {
    if (dentry) dput(dentry);
    return -ENOENT;
  }

  if (!current->fs) {
    current->fs = copy_fs_struct(nullptr);
  }

  spinlock_lock(&current->fs->lock);
  struct dentry *old = current->fs->pwd;
  current->fs->pwd = dentry; // lookup already gave us a ref
  spinlock_unlock(&current->fs->lock);

  dput(old);
  return 0;
}

EXPORT_SYMBOL(sys_chdir);

static int get_dentry_path(struct dentry *dentry, char *buf, size_t size) {
  if (dentry == root_dentry) {
    if (size < 2) return -ERANGE;
    strcpy(buf, "/");
    return 0;
  }

  /* Recursive path building (simplified) */
  struct dentry *curr = dentry;
  struct dentry *stack[32];
  int depth = 0;

  while (curr && curr != root_dentry && depth < 32) {
    stack[depth++] = curr;
    curr = curr->d_parent;
  }

  size_t offset = 0;
  for (int i = depth - 1; i >= 0; i--) {
    size_t len = strlen((const char *) stack[i]->d_name.name);
    if (offset + len + 2 > size) return -ERANGE;
    buf[offset++] = '/';
    strcpy(buf + offset, (const char *) stack[i]->d_name.name);
    offset += len;
  }
  buf[offset] = '\0';
  return 0;
}

char *sys_getcwd(char *buf_user, size_t size) {
  char *kbuf = kmalloc(size);
  if (!kbuf) return nullptr;

  if (!current->fs || !current->fs->pwd) {
    if (size < 2) {
      kfree(kbuf);
      return nullptr;
    }
    strcpy(kbuf, "/");
  } else {
    if (get_dentry_path(current->fs->pwd, kbuf, size) < 0) {
      kfree(kbuf);
      return nullptr;
    }
  }

  if (copy_to_user(buf_user, kbuf, strlen(kbuf) + 1) != 0) {
    kfree(kbuf);
    return nullptr;
  }

  kfree(kbuf);
  return buf_user;
}

EXPORT_SYMBOL(sys_getcwd);

struct getdents_callback {
  struct dir_context ctx;
  struct linux_dirent64 *dirent;
  int count;
  int error;
};

static int filldir64(struct dir_context *ctx, const char *name, int namlen,
                     vfs_loff_t offset, vfs_ino_t ino, unsigned int d_type) {
  struct getdents_callback *buf = container_of(ctx, struct getdents_callback, ctx);
  int reclen = (sizeof(struct linux_dirent64) + namlen + 1 + 7) & ~7;

  if (reclen > buf->count) return -EINVAL;

  struct linux_dirent64 *de = buf->dirent;
  de->d_ino = ino;
  de->d_off = offset;
  de->d_reclen = (unsigned short) reclen;
  de->d_type = (unsigned char) d_type;
  memcpy(de->d_name, name, namlen);
  de->d_name[namlen] = '\0';

  buf->dirent = (void *) ((uintptr_t) buf->dirent + reclen);
  buf->count -= reclen;
  return 0;
}

int sys_getdents64(unsigned int fd, struct linux_dirent64 *dirent, unsigned int count) {
  struct file *file = fget(fd);
  if (!file) return -EBADF;

  if (!file->f_op || !file->f_op->iterate) {
    fput(file);
    return -ENOTDIR;
  }

  struct getdents_callback buf = {
    .ctx.actor = filldir64,
    .ctx.pos = file->f_pos,
    .dirent = dirent,
    .count = (int) count,
  };

  int ret = file->f_op->iterate(file, &buf.ctx);
  file->f_pos = buf.ctx.pos;
  fput(file);

  if (ret < 0) return ret;
  return (int) count - buf.count;
}

EXPORT_SYMBOL(sys_getdents64);

int sys_mkdir(const char *path_user, vfs_mode_t mode) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  int ret = do_mkdir(path, mode);
  kfree(path);
  return ret;
}

EXPORT_SYMBOL(sys_mkdir);

int sys_mknod(const char *path_user, vfs_mode_t mode, dev_t dev) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  int ret = do_mknod(path, mode, dev);
  kfree(path);
  return ret;
}

EXPORT_SYMBOL(sys_mknod);

int sys_unlink(const char *path_user) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  int ret = do_unlink(path);
  kfree(path);
  return ret;
}

EXPORT_SYMBOL(sys_unlink);

int sys_rmdir(const char *path_user) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  int ret = do_rmdir(path);
  kfree(path);
  return ret;
}

EXPORT_SYMBOL(sys_rmdir);

int sys_rename(const char *oldpath_user, const char *newpath_user) {
  char *oldpath = kmalloc(4096);
  char *newpath = kmalloc(4096);
  if (!oldpath || !newpath) {
    if (oldpath) kfree(oldpath);
    if (newpath) kfree(newpath);
    return -ENOMEM;
  }

  if (copy_from_user(oldpath, oldpath_user, 4096) != 0 ||
      copy_from_user(newpath, newpath_user, 4096) != 0) {
    kfree(oldpath);
    kfree(newpath);
    return -EFAULT;
  }

  int ret = do_rename(oldpath, newpath);
  kfree(oldpath);
  kfree(newpath);
  return ret;
}

EXPORT_SYMBOL(sys_rename);

int sys_symlink(const char *oldpath_user, const char *newpath_user) {
  char *oldpath = kmalloc(4096);
  char *newpath = kmalloc(4096);
  if (!oldpath || !newpath) {
    if (oldpath) kfree(oldpath);
    if (newpath) kfree(newpath);
    return -ENOMEM;
  }

  if (copy_from_user(oldpath, oldpath_user, 4096) != 0 ||
      copy_from_user(newpath, newpath_user, 4096) != 0) {
    kfree(oldpath);
    kfree(newpath);
    return -EFAULT;
  }

  int ret = do_symlink(oldpath, newpath);
  kfree(oldpath);
  kfree(newpath);
  return ret;
}

EXPORT_SYMBOL(sys_symlink);

ssize_t sys_readlink(const char *path_user, char *buf_user, size_t bufsiz) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  char *kbuf = kmalloc(bufsiz);
  if (!kbuf) {
    kfree(path);
    return -ENOMEM;
  }

  ssize_t ret = do_readlink(path, kbuf, bufsiz);
  if (ret >= 0) {
    if (copy_to_user(buf_user, kbuf, ret) != 0) {
      ret = -EFAULT;
    }
  }

  kfree(kbuf);
  kfree(path);
  return ret;
}

EXPORT_SYMBOL(sys_readlink);

int sys_chmod(const char *path_user, vfs_mode_t mode) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  struct dentry *dentry = vfs_path_lookup(path, LOOKUP_FOLLOW);
  kfree(path);
  if (!dentry) return -ENOENT;

  struct inode *inode = dentry->d_inode;
  int ret = -EPERM;

  if (inode->i_op && inode->i_op->setattr) {
    ret = inode->i_op->setattr(dentry, mode, -1);
  } else {
    /* Generic implementation if no setattr */
    inode->i_mode = (inode->i_mode & S_IFMT) | (mode & ~S_IFMT);
    ret = 0;
  }

  if (ret == 0) vfs_notify_change(dentry, VFS_EVENT_ATTRIB);

  dput(dentry);
  return ret;
}

EXPORT_SYMBOL(sys_chmod);

int sys_chown(const char *path_user, uid_t owner, gid_t group) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  struct dentry *dentry = vfs_path_lookup(path, LOOKUP_FOLLOW);
  kfree(path);
  if (!dentry) return -ENOENT;

  struct inode *inode = dentry->d_inode;

  /* For now, just set them directly if no specialized op */
  if (owner != (uid_t) -1) inode->i_uid = owner;
  if (group != (gid_t) -1) inode->i_gid = group;

  vfs_notify_change(dentry, VFS_EVENT_ATTRIB);
  dput(dentry);
  return 0;
}

EXPORT_SYMBOL(sys_chown);

int sys_truncate(const char *path_user, vfs_loff_t length) {
  char *path = kmalloc(4096);
  if (!path) return -ENOMEM;

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    return -EFAULT;
  }

  struct dentry *dentry = vfs_path_lookup(path, LOOKUP_FOLLOW);
  kfree(path);
  if (!dentry) return -ENOENT;

  struct inode *inode = dentry->d_inode;
  int ret = -EPERM;

  if (inode->i_op && inode->i_op->setattr) {
    ret = inode->i_op->setattr(dentry, -1, length);
  } else {
    inode->i_size = length;
    ret = 0;
  }

  if (ret == 0) vfs_notify_change(dentry, VFS_EVENT_MODIFY);

  dput(dentry);
  return ret;
}

EXPORT_SYMBOL(sys_truncate);

int sys_ftruncate(int fd, vfs_loff_t length) {
  struct file *file = fget(fd);
  if (!file) return -EBADF;

  struct inode *inode = file->f_inode;
  int ret = -EPERM;

  if (inode->i_op && inode->i_op->setattr) {
    ret = inode->i_op->setattr(file->f_dentry, -1, length);
  } else {
    inode->i_size = length;
    ret = 0;
  }

  if (ret == 0) vfs_notify_change(file->f_dentry, VFS_EVENT_MODIFY);

  fput(file);
  return ret;
}

EXPORT_SYMBOL(sys_ftruncate);

int sys_mount(const char *dev_name_user, const char *dir_name_user, const char *type_user,
              unsigned long flags, void *data_user) {
  char *dev_name = nullptr;
  char *dir_name = nullptr;
  char *type = nullptr;

  if (dev_name_user) {
    dev_name = kmalloc(4096);
    if (copy_from_user(dev_name, dev_name_user, 4096) != 0) goto out_fault;
  }

  dir_name = kmalloc(4096);
  if (copy_from_user(dir_name, dir_name_user, 4096) != 0) goto out_fault;

  type = kmalloc(64);
  if (copy_from_user(type, type_user, 64) != 0) goto out_fault;

  int ret = vfs_mount(dev_name, dir_name, type, flags, data_user);
  // data is usually opaque or kernel-internal for now

  if (dev_name) kfree(dev_name);
  kfree(dir_name);
  kfree(type);
  return ret;

out_fault:
  if (dev_name) kfree(dev_name);
  if (dir_name) kfree(dir_name);
  if (type) kfree(type);
  return -EFAULT;
}

EXPORT_SYMBOL(sys_mount);

struct file *__no_cfi vfs_open(const char *path, int flags, int mode) {
  struct dentry *dentry = vfs_path_lookup(path, 0);

  if (!dentry || !dentry->d_inode) {
    if (!(flags & O_CREAT)) {
      if (dentry) dput(dentry);
      return nullptr;
    }

    struct dentry *parent = nullptr;

    if (dentry && dentry->d_parent) {
      /* Reuse the negative dentry found by lookup */
      parent = dget(dentry->d_parent);
    } else {
      /* Fallback to manual parent lookup and dentry allocation */
      if (dentry) dput(dentry);
      parent = vfs_path_lookup(path, LOOKUP_PARENT);
      if (!parent || !parent->d_inode) {
        if (parent) dput(parent);
        return nullptr;
      }

      const char *last_slash = strrchr(path, '/');
      const char *filename = last_slash ? last_slash + 1 : path;
      struct qstr qname = {.name = (const unsigned char *) filename, .len = (uint32_t) strlen(filename)};
      dentry = d_alloc_pseudo(parent->d_inode->i_sb, &qname);
      if (!dentry) {
        dput(parent);
        return nullptr;
      }
      dentry->d_parent = parent;
    }

    int ret = vfs_create(parent->d_inode, dentry, mode);
    dput(parent);

    if (ret < 0) {
      dput(dentry);
      return nullptr;
    }
  } else {
    if ((flags & O_CREAT) && (flags & O_EXCL)) {
      dput(dentry);
      return nullptr; /* -EEXIST */
    }
  }

  struct inode *inode = dentry->d_inode;
  if (!inode) {
    dput(dentry);
    return nullptr;
  }

  struct file *file = kzalloc(sizeof(struct file));
  if (!file) {
    dput(dentry);
    return nullptr;
  }

  file->f_dentry = dentry;
  file->f_inode = inode;
  file->f_op = inode->i_fop;
  file->f_flags = flags;
  file->f_pos = 0;
  atomic_set(&file->f_count, 1);

  if (file->f_op && file->f_op->open) {
    int ret = file->f_op->open(inode, file);
    if (ret < 0) {
      kfree(file);
      return nullptr;
    }
  }

  return file;
}

EXPORT_SYMBOL(vfs_open);

extern ssize_t filemap_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos);

extern ssize_t filemap_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos);

ssize_t __no_cfi vfs_read(struct file *file, char *buf, size_t count, vfs_loff_t *pos) {
  if (!file) return -EBADF;
  ssize_t ret = -1;

  /* ResDomain IO Throttling */
  struct resdomain *rd = nullptr;
  if (current && current->rd) rd = current->rd;
  else if (file->f_inode && file->f_inode->i_sb) rd = file->f_inode->i_sb->s_resdomain;

  if (rd && resdomain_io_throttle(rd, count) < 0) return -EAGAIN;

  if (file->f_op && file->f_op->read)
    ret = file->f_op->read(file, buf, count, pos);
  else if (file->f_inode && file->f_inode->i_ubc)
    /* Fallback to Page Cache (Buffered I/O) */
    ret = filemap_read(file, buf, count, pos);

  if (ret > 0 && file->f_inode) {
    file->f_inode->i_atime = current_time(file->f_inode);
  }

  return ret;
}

EXPORT_SYMBOL(vfs_read);

int vfs_mmap(struct file *file, struct vm_area_struct *vma) {
  if (!file || !file->f_op || !file->f_op->mmap) return -ENODEV;
  return file->f_op->mmap(file, vma);
}

EXPORT_SYMBOL(vfs_mmap);

ssize_t __no_cfi vfs_write(struct file *file, const char *buf, size_t count, vfs_loff_t *pos) {
  if (!file) return -EINVAL;
  ssize_t ret = -1;

  /* ResDomain IO Throttling */
  struct resdomain *rd = nullptr;
  if (current && current->rd) rd = current->rd;
  else if (file->f_inode && file->f_inode->i_sb) rd = file->f_inode->i_sb->s_resdomain;

  if (rd && resdomain_io_throttle(rd, count) < 0) return -EAGAIN;

  if (file->f_op && file->f_op->write)
    ret = file->f_op->write(file, buf, count, pos);
  else if (file->f_inode && file->f_inode->i_ubc)
    ret = filemap_write(file, buf, count, pos);

  if (ret > 0 && file->f_inode) {
    file->f_inode->i_mtime = file->f_inode->i_ctime = current_time(file->f_inode);
    vfs_notify_change(file->f_dentry, VFS_EVENT_MODIFY);
  }

  return ret;
}

EXPORT_SYMBOL(vfs_write);

int __no_cfi vfs_close(struct file *file) {
  if (!file) return -EINVAL;
  if (file->f_op && file->f_op->release) {
    file->f_op->release(file->f_inode, file);
  }
  kfree(file);
  return 0;
}

EXPORT_SYMBOL(vfs_close);

vfs_loff_t vfs_llseek(struct file *file, vfs_loff_t offset, int whence) {
  if (!file) return -EINVAL;
  if (file->f_op && file->f_op->llseek) {
    return file->f_op->llseek(file, offset, whence);
  }

  // Default implementation
  vfs_loff_t new_pos = file->f_pos;
  switch (whence) {
    case 0: // SEEK_SET
      new_pos = offset;
      break;
    case 1: // SEEK_CUR
      new_pos += offset;
      break;
    case 2: // SEEK_END
      new_pos = file->f_inode->i_size + offset;
      break;
    default:
      return -EINVAL;
  }

  if (new_pos < 0) return -EINVAL;
  file->f_pos = new_pos;
  return new_pos;
}

EXPORT_SYMBOL(vfs_llseek);

int __no_cfi vfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  if (!file) return -EBADF;

  if (file->f_op && file->f_op->ioctl) {
    return file->f_op->ioctl(file, cmd, arg);
  }

  return -ENOTTY;
}

EXPORT_SYMBOL(vfs_ioctl);

uint32_t vfs_poll(struct file *file, poll_table *pt) {
  if (!file) return POLLNVAL;
  if (file->f_op && file->f_op->poll) {
    return file->f_op->poll(file, pt);
  }

  /* Default: always readable/writable if no poll op */
  return POLLIN | POLLOUT;
}

EXPORT_SYMBOL(vfs_poll);

int vfs_stat(const char *path, struct stat *statbuf) {
  struct dentry *dentry = vfs_path_lookup(path, 0);
  if (!dentry) return -ENOENT;

  struct inode *inode = dentry->d_inode;
  if (!inode) {
    dput(dentry);
    return -ENOENT;
  }

  memset(statbuf, 0, sizeof(struct stat));
  statbuf->st_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
  statbuf->st_ino = inode->i_ino;
  statbuf->st_mode = inode->i_mode;
  statbuf->st_nlink = inode->i_nlink;
  statbuf->st_uid = inode->i_uid;
  statbuf->st_gid = inode->i_gid;
  statbuf->st_rdev = inode->i_rdev;
  statbuf->st_size = inode->i_size;
  statbuf->st_atim = inode->i_atime;
  statbuf->st_mtim = inode->i_mtime;
  statbuf->st_ctim = inode->i_ctime;
  statbuf->st_blksize = 4096;
  statbuf->st_blocks = (inode->i_size + 511) / 512;

  dput(dentry);
  return 0;
}

EXPORT_SYMBOL(vfs_stat);

int vfs_fstat(struct file *file, struct stat *statbuf) {
  if (!file || !file->f_inode) return -EBADF;
  struct inode *inode = file->f_inode;

  memset(statbuf, 0, sizeof(struct stat));
  statbuf->st_dev = inode->i_sb ? inode->i_sb->s_dev : 0;
  statbuf->st_ino = inode->i_ino;
  statbuf->st_mode = inode->i_mode;
  statbuf->st_nlink = inode->i_nlink;
  statbuf->st_uid = inode->i_uid;
  statbuf->st_gid = inode->i_gid;
  statbuf->st_rdev = inode->i_rdev;
  statbuf->st_size = inode->i_size;
  statbuf->st_atim = inode->i_atime;
  statbuf->st_mtim = inode->i_mtime;
  statbuf->st_ctim = inode->i_ctime;
  statbuf->st_blksize = 4096;
  statbuf->st_blocks = (inode->i_size + 511) / 512;

  return 0;
}

EXPORT_SYMBOL(vfs_fstat);

// Function to register a new filesystem type
int register_filesystem(struct file_system_type *fs) {
  if (!fs || !fs->name || !fs->mount || !fs->kill_sb) {
    printk(KERN_ERR VFS_CLASS "Attempted to register an invalid filesystem type.\n");
    return -EINVAL;
  }
  mutex_lock(&fs_type_mutex);
  list_add_tail(&fs->fs_list, &file_systems);
  mutex_unlock(&fs_type_mutex);
  printk(VFS_CLASS "Registered filesystem: %s\n", fs->name);
  return 0;
}

EXPORT_SYMBOL(register_filesystem);

// Function to unregister a filesystem type
int unregister_filesystem(struct file_system_type *fs) {
  if (!fs) {
    printk(KERN_ERR VFS_CLASS "Attempted to unregister a nullptr filesystem type.\n");
    return -EINVAL;
  }
  mutex_lock(&fs_type_mutex);
  list_del(&fs->fs_list);
  mutex_unlock(&fs_type_mutex);
  printk(VFS_CLASS "Unregistered filesystem: %s\n", fs->name);
  return 0;
}

EXPORT_SYMBOL(unregister_filesystem);

/*
 * Default operations for character devices
 */
static int __no_cfi chrdev_open(struct inode *inode, struct file *file) {
  struct char_device *cdev = chrdev_lookup(inode->i_rdev);
  if (!cdev) return -ENODEV;

  file->private_data = cdev;

  // If the driver has an open function, call it
  if (cdev->ops && cdev->ops->open) {
    return cdev->ops->open(cdev);
  }

  return 0;
}

static int __no_cfi chrdev_release(struct inode *inode, struct file *file) {
  struct char_device *cdev = file->private_data;
  if (cdev && cdev->ops && cdev->ops->close) {
    cdev->ops->close(cdev);
  }
  return 0;
}

static ssize_t __no_cfi chrdev_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->read) return -EINVAL;
  return cdev->ops->read(cdev, buf, count, ppos);
}

static ssize_t __no_cfi chrdev_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->write) return -EINVAL;
  return cdev->ops->write(cdev, buf, count, ppos);
}

static int __no_cfi chrdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->ioctl) return -ENOTTY;
  return cdev->ops->ioctl(cdev, cmd, (void *) arg);
}

static int __no_cfi chrdev_mmap(struct file *file, struct vm_area_struct *vma) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->mmap) return -ENODEV;
  return cdev->ops->mmap(cdev, vma);
}

static int blkdev_open(struct inode *inode, struct file *file) {
  struct block_device *bdev = blkdev_lookup(inode->i_rdev);
  if (!bdev) return -ENODEV;

  file->private_data = bdev;
  return 0;
}

static int blkdev_release(struct inode *inode, struct file *file) {
  struct block_device *bdev = file->private_data;
  if (bdev) {
    put_device(&bdev->dev);
  }
  return 0;
}

static ssize_t blkdev_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct block_device *bdev = file->private_data;
  if (!bdev) return -EINVAL;

  uint64_t start_sector = (*ppos) / bdev->block_size;
  uint32_t sector_count = (count + bdev->block_size - 1) / bdev->block_size;

  /* Basic bounce buffer for now (aligned I/O) */
  void *kbuf = kmalloc(sector_count * bdev->block_size);
  if (!kbuf) return -ENOMEM;

  int ret = block_read(bdev, kbuf, start_sector, sector_count);
  if (ret == 0) {
    size_t to_copy = (count < sector_count * bdev->block_size) ? count : sector_count * bdev->block_size;
    copy_to_user(buf, kbuf, to_copy);
    *ppos += to_copy;
    ret = to_copy;
  } else {
    ret = -EIO;
  }

  kfree(kbuf);
  return ret;
}

static ssize_t blkdev_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  struct block_device *bdev = file->private_data;
  if (!bdev) return -EINVAL;

  uint64_t start_sector = (*ppos) / bdev->block_size;
  uint32_t sector_count = (count + bdev->block_size - 1) / bdev->block_size;

  void *kbuf = kmalloc(sector_count * bdev->block_size);
  if (!kbuf) return -ENOMEM;

  if (copy_from_user(kbuf, buf, count) != 0) {
    kfree(kbuf);
    return -EFAULT;
  }

  int ret = block_write(bdev, kbuf, start_sector, sector_count);
  if (ret == 0) {
    *ppos += count;
    ret = count;
  } else {
    ret = -EIO;
  }

  kfree(kbuf);
  return ret;
}

static struct file_operations def_blk_fops = {
  .open = blkdev_open,
  .release = blkdev_release,
  .read = blkdev_read,
  .write = blkdev_write,
};

static struct file_operations def_chr_fops = {
  .open = chrdev_open,
  .release = chrdev_release,
  .read = chrdev_read,
  .write = chrdev_write,
  .ioctl = chrdev_ioctl,
  .mmap = chrdev_mmap,
};

static struct file_operations def_fifo_fops = {
  .open = nullptr, // TODO: pipe_open
};

static struct file_operations def_sock_fops = {
  .open = nullptr, // TODO: socket_open
};

void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev) {
  inode->i_mode = mode;
  if (S_ISCHR(mode)) {
    inode->i_fop = &def_chr_fops;
    inode->i_rdev = rdev;
  } else if (S_ISBLK(mode)) {
    inode->i_fop = &def_blk_fops;
    inode->i_rdev = rdev;
  } else if (S_ISFIFO(mode)) {
    inode->i_fop = &def_fifo_fops;
  } else if (S_ISSOCK(mode)) {
    inode->i_fop = &def_sock_fops;
  }
}

EXPORT_SYMBOL(init_special_inode);

struct timespec current_time(struct inode *inode) {
  struct timespec now;
  (void) inode;
  ktime_get_real_ts64(&now);
  return now;
}

EXPORT_SYMBOL(current_time);

struct inode *new_inode(struct super_block *sb) {
  struct inode *inode = kzalloc(sizeof(struct inode));
  if (!inode) return nullptr;

  inode->i_sb = sb;
  spinlock_init(&inode->i_lock);
  INIT_LIST_HEAD(&inode->i_list);
  INIT_LIST_HEAD(&inode->i_dentry);
  init_waitqueue_head(&inode->i_wait);
  atomic_set(&inode->i_count, 1);

  mutex_lock(&inode_mutex);
  list_add_tail(&inode->i_list, &inodes);
  mutex_unlock(&inode_mutex);

  return inode;
}

EXPORT_SYMBOL(new_inode);

void iget(struct inode *inode) {
  if (inode) {
    atomic_inc(&inode->i_count);
  }
}

EXPORT_SYMBOL(iget);

void iput(struct inode *inode) {
  if (!inode) return;

  if (atomic_dec_and_test(&inode->i_count)) {
    mutex_lock(&inode_mutex);
    list_del(&inode->i_list);
    mutex_unlock(&inode_mutex);

    if (inode->i_ubc) {
      vm_object_put(inode->i_ubc);
    }

    if (inode->i_sb && inode->i_sb->s_op && inode->i_sb->s_op->destroy_inode) {
      inode->i_sb->s_op->destroy_inode(inode);
    } else {
      kfree(inode);
    }
  }
}

EXPORT_SYMBOL(iput);

struct dentry *dget(struct dentry *dentry) {
  if (dentry) {
    atomic_inc(&dentry->d_count);
  }
  return dentry;
}

EXPORT_SYMBOL(dget);

void dput(struct dentry *dentry) {
  if (!dentry) return;

  if (atomic_dec_and_test(&dentry->d_count)) {
    /* Remove from parent's subdirectory list */
    if (!list_empty(&dentry->d_child)) {
      list_del(&dentry->d_child);
    }

    /* Remove from inode's dentry list */
    if (!list_empty(&dentry->i_list)) {
      list_del(&dentry->i_list);
    }

    if (dentry->d_inode) {
      iput(dentry->d_inode);
    }
    kfree((void *) dentry->d_name.name);
    kfree(dentry);
  }
}

EXPORT_SYMBOL(dput);

ssize_t simple_read_from_buffer(void *to, size_t count, vfs_loff_t *ppos, const void *from, size_t available) {
  vfs_loff_t pos = *ppos;
  if (pos < 0) return -EINVAL;
  if (pos >= available || !count) return 0;
  if (count > available - pos) count = available - pos;
  if (copy_to_user(to, from + pos, count)) return -EFAULT;
  *ppos = pos + count;
  return count;
}

EXPORT_SYMBOL(simple_read_from_buffer);

struct dentry *simple_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) dir;
  (void) flags;
  return dentry;
}

EXPORT_SYMBOL(simple_lookup);

int simple_rmdir(struct inode *dir, struct dentry *dentry) {
  (void) dir;
  (void) dentry;
  return 0;
}

EXPORT_SYMBOL(simple_rmdir);
