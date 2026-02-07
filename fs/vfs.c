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
#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/block.h>
#include <fs/devfs.h>
#include <fs/fs_struct.h>
#include <aerosync/timer.h>
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

LIST_HEAD(mount_list);
static struct mutex mount_mutex;

LIST_HEAD(file_systems); // List of all registered file system types
static struct mutex fs_type_mutex;

extern struct dentry *root_dentry;
extern struct file_system_type tmpfs_type;

void vfs_init(void) {
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

  // Mount tmpfs as the base rootfs
  vfs_mount(nullptr, "/", "tmpfs", 0, nullptr);

  /* Initialize current task's filesystem context */
  if (current) {
    current->fs = kzalloc(sizeof(struct fs_struct));
    if (current->fs) {
      atomic_set(&current->fs->count, 1);
      spinlock_init(&current->fs->lock);
      current->fs->root = dget(root_dentry);
      current->fs->pwd = dget(root_dentry);
    }
  }
#ifdef CONFIG_DEVFS
  devfs_init();

#ifdef CONFIG_DEVFS_MOUNT
  printk(VFS_CLASS "Automounting devfs at %s\n", STRINGIFY(CONFIG_DEVFS_MOUNT_PATH));
  vfs_mount(nullptr, STRINGIFY(CONFIG_DEVFS_MOUNT_PATH), "devfs", 0, nullptr);
#endif
#endif

  printk(VFS_CLASS "VFS initialization complete.\n");
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
          mutex_unlock(&fs_type_mutex);
          return -ENOENT;
        }
      }

      ret = fs->mount(fs, dev_name, dir_name, flags, data);
      if (ret == 0) {
        /* Record the mount */
        struct mount *mnt = kzalloc(sizeof(struct mount));
        struct super_block *sb = list_last_entry(&super_blocks, struct super_block, sb_list);

        mnt->mnt_sb = sb;
        mnt->mnt_root = dget(sb->s_root);
        mnt->mnt_mountpoint = mountpoint; // lookup returned a ref

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

struct file *vfs_open(const char *path, int flags, int mode) {
  struct dentry *dentry = vfs_path_lookup(path, 0);
  if (!dentry) return nullptr;

  struct inode *inode = dentry->d_inode;
  if (!inode) return nullptr;

  struct file *file = kzalloc(sizeof(struct file));
  if (!file) return nullptr;

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

ssize_t vfs_read(struct file *file, char *buf, size_t count, vfs_loff_t *pos) {
  if (!file) return -1;
  ssize_t ret = -1;

  if (file->f_op && file->f_op->read)
    ret = file->f_op->read(file, buf, count, pos);
  else if (file->f_inode && file->f_inode->i_mapping)
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

ssize_t vfs_write(struct file *file, const char *buf, size_t count, vfs_loff_t *pos) {
  if (!file) return -1;
  ssize_t ret = -1;

  if (file->f_op && file->f_op->write)
    ret = file->f_op->write(file, buf, count, pos);
  else if (file->f_inode && file->f_inode->i_mapping)
    ret = filemap_write(file, buf, count, pos);

  if (ret > 0 && file->f_inode) {
    file->f_inode->i_mtime = file->f_inode->i_ctime = current_time(file->f_inode);
  }

  return ret;
}

EXPORT_SYMBOL(vfs_write);

int vfs_close(struct file *file) {
  if (!file) return -1;
  if (file->f_op && file->f_op->release) {
    file->f_op->release(file->f_inode, file);
  }
  kfree(file);
  return 0;
}

EXPORT_SYMBOL(vfs_close);

vfs_loff_t vfs_llseek(struct file *file, vfs_loff_t offset, int whence) {
  if (!file) return -1;
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
      return -1;
  }

  if (new_pos < 0) return -1;
  file->f_pos = new_pos;
  return new_pos;
}

EXPORT_SYMBOL(vfs_llseek);

int vfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
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
  statbuf->st_dev = 0; // TODO: sb->s_dev
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
  statbuf->st_dev = 0;
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
    return -1;
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
    return -1;
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
static int chrdev_open(struct inode *inode, struct file *file) {
  struct char_device *cdev = chrdev_lookup(inode->i_rdev);
  if (!cdev) return -ENODEV;

  file->private_data = cdev;

  // If the driver has an open function, call it
  if (cdev->ops && cdev->ops->open) {
    return cdev->ops->open(cdev);
  }

  return 0;
}

static int chrdev_release(struct inode *inode, struct file *file) {
  struct char_device *cdev = file->private_data;
  if (cdev && cdev->ops && cdev->ops->close) {
    cdev->ops->close(cdev);
  }
  return 0;
}

static ssize_t chrdev_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->read) return -EINVAL;
  return cdev->ops->read(cdev, buf, count, ppos);
}

static ssize_t chrdev_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  struct char_device *cdev = file->private_data;
  if (!cdev || !cdev->ops || !cdev->ops->write) return -EINVAL;
  return cdev->ops->write(cdev, buf, count, ppos);
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
};

void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev) {
  inode->i_mode = mode;
  if (S_ISCHR(mode)) {
    inode->i_fop = &def_chr_fops;
    inode->i_rdev = rdev;
  } else if (S_ISBLK(mode)) {
    inode->i_fop = &def_blk_fops;
    inode->i_rdev = rdev;
  } else if (S_ISFIFO(mode) || S_ISSOCK(mode)) {
    inode->i_fop = nullptr; // TODO
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

    if (inode->i_mapping) {
      vm_object_put(inode->i_mapping);
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
    /* Simplified dentry reclamation */
    if (dentry->d_inode) {
      iput(dentry->d_inode);
    }
    kfree((void *) dentry->d_name.name);
    kfree(dentry);
  }
}

EXPORT_SYMBOL(dput);
