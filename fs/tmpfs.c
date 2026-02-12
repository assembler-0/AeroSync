/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/tmpfs.c
 * @brief Advanced Temporary Filesystem (Linux-style tmpfs) with ResDomain Accounting
 * @copyright (C) 2026 assembler-0
 */

#include <fs/vfs.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <aerosync/errno.h>
#include <aerosync/timer.h>
#include <aerosync/atomic.h>
#include <lib/string.h>
#include <aerosync/resdomain.h>
#include <lib/uaccess.h>

#define TMPFS_MAGIC 0x01021994

static atomic_t tmpfs_ino_counter = ATOMIC_INIT(1);

static struct inode_operations tmpfs_dir_inode_ops;
static struct inode_operations tmpfs_file_inode_ops;
static struct file_operations tmpfs_file_operations;
static struct file_operations tmpfs_dir_operations;

struct tmpfs_inode_info {
  struct resdomain *rd;
  char *symlink_target;
};

static struct resdomain *tmpfs_get_domain(struct super_block *sb) {
  if (sb && sb->s_resdomain) return sb->s_resdomain;
  struct task_struct *curr = get_current();
  return curr ? curr->rd : &root_resdomain;
}

struct inode *tmpfs_get_inode(struct super_block *sb, const struct inode *dir, vfs_mode_t mode, dev_t dev) {
  (void) dir;
  struct resdomain *rd = tmpfs_get_domain(sb);

  if (resdomain_charge_mem(rd, sizeof(struct inode) + sizeof(struct tmpfs_inode_info), false) < 0) {
    return nullptr;
  }

  struct inode *inode = new_inode(sb);
  if (!inode) {
    resdomain_uncharge_mem(rd, sizeof(struct inode) + sizeof(struct tmpfs_inode_info));
    return nullptr;
  }

  struct tmpfs_inode_info *info = kzalloc(sizeof(struct tmpfs_inode_info));
  if (!info) {
    resdomain_uncharge_mem(rd, sizeof(struct inode) + sizeof(struct tmpfs_inode_info));
    iput(inode);
    return nullptr;
  }

  inode->i_ino = atomic_inc_return(&tmpfs_ino_counter);
  inode->i_mode = mode;
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

  info->rd = rd;
  resdomain_get(rd);
  inode->i_fs_info = info;

  if (S_ISREG(mode)) {
    inode->i_op = &tmpfs_file_inode_ops;
    inode->i_fop = &tmpfs_file_operations;
    inode->i_mapping = vm_object_alloc(VM_OBJECT_FILE);
    if (inode->i_mapping) {
      inode->i_mapping->priv = inode;
      inode->i_mapping->flags |= VM_OBJECT_SWAP_BACKED;
      inode->i_mapping->rd = rd;
      resdomain_get(rd);

      extern const struct vm_object_operations filemap_obj_ops;
      inode->i_mapping->ops = &filemap_obj_ops;
    }
  } else if (S_ISDIR(mode)) {
    inode->i_op = &tmpfs_dir_inode_ops;
    inode->i_fop = &tmpfs_dir_operations;
  } else if (S_ISCHR(mode) || S_ISBLK(mode)) {
    init_special_inode(inode, mode, dev);
  }

  return inode;
}

static int tmpfs_iterate(struct file *file, struct dir_context *ctx) {
  struct inode *inode = file->f_inode;
  struct dentry *dentry = file->f_dentry;

  if (ctx->pos == 0) {
    if (ctx->actor(ctx, ".", 1, 0, inode->i_ino, DT_DIR) < 0) return 0;
    ctx->pos = 1;
  }
  if (ctx->pos == 1) {
    vfs_ino_t p_ino = (dentry->d_parent && dentry->d_parent->d_inode) ? dentry->d_parent->d_inode->i_ino : inode->i_ino;
    if (ctx->actor(ctx, "..", 2, 1, p_ino, DT_DIR) < 0) return 0;
    ctx->pos = 2;
  }

  struct dentry *child;
  vfs_loff_t i = 2;

  spinlock_lock(&dentry->d_lock);
  list_for_each_entry(child, &dentry->d_subdirs, d_child) {
    if (i >= ctx->pos) {
      unsigned int type = DT_UNKNOWN;
      if (child->d_inode) {
        vfs_mode_t mode = child->d_inode->i_mode;
        if (S_ISDIR(mode)) type = DT_DIR;
        else if (S_ISREG(mode)) type = DT_REG;
        else if (S_ISLNK(mode)) type = DT_LNK;
        else if (S_ISCHR(mode)) type = DT_CHR;
        else if (S_ISBLK(mode)) type = DT_BLK;
      }

      spinlock_unlock(&dentry->d_lock);
      if (ctx->actor(ctx, (const char *) child->d_name.name, (int) child->d_name.len,
                     i, child->d_inode ? child->d_inode->i_ino : 0, type) < 0)
        return 0;
      spinlock_lock(&dentry->d_lock);

      ctx->pos = i + 1;
    }
    i++;
  }
  spinlock_unlock(&dentry->d_lock);
  return 0;
}

static struct dentry *tmpfs_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) dir;
  (void) flags;
  return dentry;
}

static int tmpfs_mkmknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
  struct inode *inode = tmpfs_get_inode(dir->i_sb, dir, mode, dev);
  if (!inode) return -ENOMEM;

  dentry->d_inode = inode;
  list_add(&dentry->i_list, &inode->i_dentry);
  dir->i_mtime = dir->i_ctime = current_time(dir);
  return 0;
}

static int tmpfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_mkmknod(dir, dentry, mode | S_IFDIR, 0);
}

static int tmpfs_create(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_mkmknod(dir, dentry, mode | S_IFREG, 0);
}

static ssize_t tmpfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz) {
  struct tmpfs_inode_info *info = dentry->d_inode->i_fs_info;
  if (!info || !info->symlink_target) return -EINVAL;
  size_t len = strlen(info->symlink_target);
  if (len > bufsiz) len = bufsiz;
  if (copy_to_user(buf, info->symlink_target, len)) return -EFAULT;
  return (ssize_t) len;
}

static const char *tmpfs_follow_link(struct dentry *dentry, void **cookie) {
  struct tmpfs_inode_info *info = dentry->d_inode->i_fs_info;
  if (!info || !info->symlink_target) return ERR_PTR(-EINVAL);
  *cookie = nullptr;
  return info->symlink_target;
}

static struct inode_operations tmpfs_symlink_inode_ops = {
  .readlink = tmpfs_readlink,
  .follow_link = tmpfs_follow_link,
};

static int tmpfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname) {
  int ret = tmpfs_mkmknod(dir, dentry, S_IFLNK | 0777, 0);
  if (ret == 0) {
    struct tmpfs_inode_info *info = dentry->d_inode->i_fs_info;
    info->symlink_target = kstrdup(oldname);
    if (!info->symlink_target) return -ENOMEM;
    resdomain_charge_mem(info->rd, strlen(info->symlink_target) + 1, false);
    dentry->d_inode->i_op = &tmpfs_symlink_inode_ops;
  }
  return ret;
}

static struct inode_operations tmpfs_dir_inode_ops = {
  .lookup = tmpfs_lookup,
  .mkdir = tmpfs_mkdir,
  .create = tmpfs_create,
  .symlink = tmpfs_symlink,
};

static struct file_operations tmpfs_dir_operations = {
  .iterate = tmpfs_iterate,
};

static struct file_operations tmpfs_file_operations = {
  .mmap = generic_file_mmap,
};

static struct inode_operations tmpfs_file_inode_ops = {
  .getattr = nullptr,
};

static void tmpfs_destroy_inode(struct inode *inode) {
  struct tmpfs_inode_info *info = inode->i_fs_info;

  if (inode->i_mapping) {
    if (inode->i_mapping->rd) resdomain_put(inode->i_mapping->rd);
    vm_object_put(inode->i_mapping);
  }

  if (info) {
    struct resdomain *rd = info->rd;
    if (info->symlink_target) {
      resdomain_uncharge_mem(rd, strlen(info->symlink_target) + 1);
      kfree(info->symlink_target);
    }
    resdomain_uncharge_mem(rd, sizeof(struct inode) + sizeof(struct tmpfs_inode_info));
    resdomain_put(rd);
    kfree(info);
  }

  kfree(inode);
}

static struct super_operations tmpfs_ops = {
  .destroy_inode = tmpfs_destroy_inode,
};

static int tmpfs_fill_super(struct super_block *sb, void *data) {
  (void) data;
  sb->s_maxbytes = 0x7fffffffffffffffULL;
  sb->s_blocksize = PAGE_SIZE;
  sb->s_magic = TMPFS_MAGIC;
  sb->s_op = &tmpfs_ops;

  struct inode *inode = tmpfs_get_inode(sb, nullptr, S_IFDIR | 0755, 0);
  if (!inode) return -ENOMEM;

  struct qstr root_name = {.name = (const unsigned char *) "/", .len = 1};
  sb->s_root = d_alloc_pseudo(sb, &root_name);
  if (!sb->s_root) {
    iput(inode);
    return -ENOMEM;
  }
  sb->s_root->d_inode = inode;
  return 0;
}

static int tmpfs_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name,
                       unsigned long flags, void *data) {
  (void) dev_name;
  (void) dir_name;
  (void) flags;
  struct super_block *sb = kzalloc(sizeof(struct super_block));
  if (!sb) return -ENOMEM;

  int ret = tmpfs_fill_super(sb, data);
  if (ret) {
    kfree(sb);
    return ret;
  }

  extern struct list_head super_blocks;
  list_add_tail(&sb->sb_list, &super_blocks);
  return 0;
}

static void tmpfs_kill_sb(struct super_block *sb) {
  kfree(sb);
}

struct file_system_type tmpfs_type = {
  .name = "tmpfs",
  .mount = tmpfs_mount,
  .kill_sb = tmpfs_kill_sb,
};

void tmpfs_init(void) {
  register_filesystem(&tmpfs_type);
}
