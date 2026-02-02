/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/tmpfs.c
 * @brief advanced Temporary Filesystem (Linux-style tmpfs)
 * @copyright (C) 2026 assembler-0
 */

#include <fs/vfs.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <aerosync/errno.h>
#include <aerosync/timer.h>
#include <fs/file.h>

#define TMPFS_MAGIC 0x01021994

static struct inode_operations tmpfs_dir_inode_ops;
static struct inode_operations tmpfs_file_inode_ops;
static struct file_operations tmpfs_file_operations;
static struct file_operations tmpfs_dir_operations;

/* forward declaration */
int generic_file_mmap(struct file *file, struct vm_area_struct *vma);

/**
 * tmpfs_get_inode - Create a new inode for tmpfs
 */
struct inode *tmpfs_get_inode(struct super_block *sb, const struct inode *dir, vfs_mode_t mode, dev_t dev) {
  struct inode *inode = new_inode(sb);
  if (!inode) return nullptr;

  inode->i_mode = mode;
  inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

  if (S_ISREG(mode)) {
    inode->i_op = &tmpfs_file_inode_ops;
    inode->i_fop = &tmpfs_file_operations;
    /* Back the file with a VM Object (Page Cache + Swap Backed) */
    inode->i_mapping = vm_object_alloc(VM_OBJECT_FILE);
    if (inode->i_mapping) {
      inode->i_mapping->priv = inode;
      inode->i_mapping->flags |= VM_OBJECT_SWAP_BACKED;
      extern const struct vm_object_operations filemap_obj_ops;
      inode->i_mapping->ops = &filemap_obj_ops;
    }
  } else if (S_ISDIR(mode)) {
    inode->i_op = &tmpfs_dir_inode_ops;
    inode->i_fop = &tmpfs_dir_operations;
  } else if (S_ISCHR(mode) || S_ISBLK(mode)) {
    extern void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev);
    init_special_inode(inode, mode, dev);
  }

  return inode;
}

/* --- Directory Operations --- */

static struct dentry *tmpfs_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) dir;
  (void) flags;
  /* In tmpfs, lookups are satisfied by the dentry cache. */
  return nullptr;
}

static int tmpfs_mkmknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
  struct inode *inode = tmpfs_get_inode(dir->i_sb, dir, mode, dev);
  if (!inode) return -ENOMEM;

  dentry->d_inode = inode;
  dir->i_mtime = dir->i_ctime = current_time(dir);
  return 0;
}

static int tmpfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_mkmknod(dir, dentry, mode | S_IFDIR, 0);
}

static int tmpfs_create(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_mkmknod(dir, dentry, mode | S_IFREG, 0);
}

static struct inode_operations tmpfs_dir_inode_ops = {
  .lookup = tmpfs_lookup,
  .mkdir = tmpfs_mkdir,
  .create = tmpfs_create,
};

static struct file_operations tmpfs_dir_operations = {
  .read = nullptr, // TODO: readdir
};

/* --- File Operations --- */

static struct file_operations tmpfs_file_operations = {
  .read = vfs_read, /* Uses Page Cache via i_mapping */
  .write = vfs_write, /* Uses Page Cache via i_mapping */
  .mmap = generic_file_mmap,
};

static struct inode_operations tmpfs_file_inode_ops = {
  .getattr = nullptr,
};

/* --- Superblock Operations --- */

static struct super_operations tmpfs_ops = {
  .statfs = nullptr,
};

static int tmpfs_fill_super(struct super_block *sb, void *data) {
  sb->s_maxbytes = 0x7fffffffffffffffULL;
  sb->s_blocksize = PAGE_SIZE;
  sb->s_magic = TMPFS_MAGIC;
  sb->s_op = &tmpfs_ops;

  struct inode *inode = tmpfs_get_inode(sb, nullptr, S_IFDIR | 0755, 0);
  if (!inode) return -ENOMEM;

  struct qstr root_name = {.name = (const unsigned char *) "/", .len = 1};
  extern struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);
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
