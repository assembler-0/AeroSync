/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/pseudo_fs.c
 * @brief Generic Pseudo-Filesystem Library (APS) - RB-Tree Optimized + Locked + ResDomain
 * @copyright (C) 2026 assembler-0
 */

#include <fs/pseudo_fs.h>
#include <fs/vfs.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <aerosync/errno.h>
#include <aerosync/atomic.h>
#include <aerosync/rw_semaphore.h>
#include <aerosync/resdomain.h>
#include <aerosync/sched/sched.h>

static atomic_t next_pseudo_ino = ATOMIC_INIT(1);

/* --- Comparison Helper --- */

static int pseudo_cmp(const void *key, const struct rb_node *node) {
  const struct pseudo_node *p = rb_entry(node, struct pseudo_node, rb_node);
  return strcmp((const char *) key, p->name);
}

/* --- Symlink Operations --- */

static ssize_t pseudo_readlink(struct dentry *dentry, char *buf, size_t bufsiz) {
  struct pseudo_node *node = dentry->d_inode->i_fs_info;
  if (!node || !node->symlink_target) return -EINVAL;
  size_t len = strlen(node->symlink_target);
  if (len > bufsiz) len = bufsiz;
  memcpy(buf, node->symlink_target, len);
  return len;
}

static const char *pseudo_follow_link(struct dentry *dentry, void **cookie) {
  struct pseudo_node *node = dentry->d_inode->i_fs_info;
  if (!node || !node->symlink_target) return ERR_PTR(-EINVAL);
  *cookie = nullptr;
  return node->symlink_target;
}

static struct inode_operations pseudo_symlink_iop = {
  .readlink = pseudo_readlink,
  .follow_link = pseudo_follow_link,
};

/* --- Directory Operations --- */

static int pseudo_iterate(struct file *file, struct dir_context *ctx) {
  struct pseudo_node *parent = file->f_inode->i_fs_info;
  if (!parent) return -ENOTDIR;

  if (ctx->pos == 0) {
    if (ctx->actor(ctx, ".", 1, 0, file->f_inode->i_ino, DT_DIR) < 0) return 0;
    ctx->pos = 1;
  }
  if (ctx->pos == 1) {
    vfs_ino_t p_ino = parent->parent ? parent->parent->i_ino : file->f_inode->i_ino;
    if (ctx->actor(ctx, "..", 2, 1, p_ino, DT_DIR) < 0) return 0;
    ctx->pos = 2;
  }

  /* Locking for iteration safety */
  down_read(&parent->lock);

  struct rb_node *n;
  vfs_loff_t i = 2;

  /*
   * Note: RB-tree iteration is O(n).
   * Ideally we would jump to the right node for 'pos', but RB-tree doesn't support random access efficiently.
   * For pseudo-fs, directory sizes are usually small.
   */
  for (n = rb_first(&parent->children); n; n = rb_next(n)) {
    if (i >= ctx->pos) {
      struct pseudo_node *node = rb_entry(n, struct pseudo_node, rb_node);
      unsigned int type = DT_UNKNOWN;
      if (S_ISDIR(node->mode)) type = DT_DIR;
      else if (S_ISREG(node->mode)) type = DT_REG;
      else if (S_ISLNK(node->mode)) type = DT_LNK;
      else if (S_ISCHR(node->mode)) type = DT_CHR;
      else if (S_ISBLK(node->mode)) type = DT_BLK;

      if (ctx->actor(ctx, node->name, (int) strlen(node->name), i,
                     node->i_ino, type) < 0) {
        up_read(&parent->lock);
        return 0;
      }
      ctx->pos = i + 1;
    }
    i++;
  }

  up_read(&parent->lock);
  return 0;
}

static struct file_operations pseudo_dir_fops = {
  .iterate = pseudo_iterate,
};

/* --- Common Inode Operations --- */

static struct dentry *pseudo_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) flags;
  struct pseudo_node *parent = dir->i_fs_info;
  struct rb_node *match;

  if (!parent) return nullptr;

  down_read(&parent->lock);
  match = rb_find((const char *) dentry->d_name.name, &parent->children, pseudo_cmp);
  if (match) {
    struct pseudo_node *node = rb_entry(match, struct pseudo_node, rb_node);
    struct inode *inode = new_inode(dir->i_sb);
    if (!inode) {
      up_read(&parent->lock);
      return nullptr;
    }

    inode->i_ino = node->i_ino;

    if (node->init_inode) {
      node->init_inode(inode, node);
    } else {
      inode->i_mode = node->mode;
      if (S_ISLNK(node->mode)) {
        inode->i_op = &pseudo_symlink_iop;
      } else if (S_ISDIR(node->mode)) {
        inode->i_op = node->iop ? node->iop : dir->i_op;
        inode->i_fop = &pseudo_dir_fops;
      } else {
        inode->i_op = node->iop;
        inode->i_fop = node->fop;
      }
    }

    inode->i_fs_info = node;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

    dentry->d_inode = inode;
    node->inode = inode;

    up_read(&parent->lock);
    return dentry;
  }
  up_read(&parent->lock);

  return nullptr;
}

static struct inode_operations pseudo_dir_iop = {
  .lookup = pseudo_lookup,
};

/* --- Registration API --- */

static int pseudo_fill_super(struct super_block *sb, void *data) {
  struct pseudo_fs_info *info = data;
  sb->s_magic = 0x50534555; /* "PSEU" */
  sb->s_blocksize = PAGE_SIZE;
  sb->s_fs_info = info;

  struct inode *root_inode = new_inode(sb);
  if (!root_inode) return -ENOMEM;

  root_inode->i_ino = info->root->i_ino;
  root_inode->i_mode = S_IFDIR | 0755;
  root_inode->i_op = &pseudo_dir_iop;
  root_inode->i_fop = &pseudo_dir_fops;
  root_inode->i_fs_info = info->root;
  root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);

  struct qstr root_name = {.name = (const unsigned char *) "/", .len = 1};
  extern struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);
  sb->s_root = d_alloc_pseudo(sb, &root_name);
  if (!sb->s_root) {
    iput(root_inode);
    return -ENOMEM;
  }
  sb->s_root->d_inode = root_inode;
  info->sb = sb;

  return 0;
}

static int pseudo_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name,
                        unsigned long flags, void *data) {
  (void) dev_name;
  (void) dir_name;
  (void) flags;
  struct pseudo_fs_info *info = container_of(fs_type, struct pseudo_fs_info, fs_type);

  struct super_block *sb = kzalloc(sizeof(struct super_block));
  if (!sb) return -ENOMEM;

  int ret = pseudo_fill_super(sb, info);
  if (ret) {
    kfree(sb);
    return ret;
  }

  extern struct list_head super_blocks;
  list_add_tail(&sb->sb_list, &super_blocks);
  return 0;
}

static void pseudo_kill_sb(struct super_block *sb) {
  if (sb) {
    if (sb->s_root) {
      dput(sb->s_root);
    }
    kfree(sb);
  }
}

int pseudo_fs_register(struct pseudo_fs_info *info) {
  if (!info || !info->name) return -EINVAL;

  info->root = kzalloc(sizeof(struct pseudo_node));
  if (!info->root) return -ENOMEM;

  strncpy(info->root->name, "/", 64);
  info->root->mode = S_IFDIR | 0755;
  info->root->children = RB_ROOT;
  info->root->i_ino = atomic_inc_return(&next_pseudo_ino);
  rwsem_init(&info->root->lock);

  /* Root is owned by root domain */
  info->root->rd = &root_resdomain;

  info->fs_type.name = info->name;
  info->fs_type.mount = pseudo_mount;
  info->fs_type.kill_sb = pseudo_kill_sb;

  return register_filesystem(&info->fs_type);
}

struct pseudo_node *pseudo_fs_find_node(struct pseudo_node *parent, const char *name) {
  if (!parent) return nullptr;
  down_read(&parent->lock);
  struct rb_node *match = rb_find(name, &parent->children, pseudo_cmp);
  up_read(&parent->lock);
  if (!match) return nullptr;
  return rb_entry(match, struct pseudo_node, rb_node);
}

struct pseudo_node *pseudo_fs_create_node(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          vfs_mode_t mode,
                                          const struct file_operations *fops,
                                          void *private_data) {
  /* Charge memory to current task's resdomain */
  struct resdomain *rd = current ? current->rd : &root_resdomain;

  if (resdomain_charge_mem(rd, sizeof(struct pseudo_node), false) < 0) {
    return nullptr;
  }

  struct pseudo_node *node = kzalloc(sizeof(struct pseudo_node));
  if (!node) {
    resdomain_uncharge_mem(rd, sizeof(struct pseudo_node));
    return nullptr;
  }

  if (!parent) parent = fs->root;

  strncpy(node->name, name, 64);
  node->mode = mode;
  node->fop = fops;
  node->private_data = private_data;
  node->parent = parent;
  node->children = RB_ROOT;
  node->i_ino = atomic_inc_return(&next_pseudo_ino);
  rwsem_init(&node->lock);
  node->rd = rd;
  resdomain_get(rd);

  down_write(&parent->lock);

  /* Insert into RB-Tree */
  struct rb_node **link = &parent->children.rb_node;
  struct rb_node *p = nullptr;

  while (*link) {
    p = *link;
    struct pseudo_node *entry = rb_entry(p, struct pseudo_node, rb_node);
    int cmp = strcmp(name, entry->name);

    if (cmp < 0)
      link = &p->rb_left;
    else if (cmp > 0)
      link = &p->rb_right;
    else {
      /* Duplicate name */
      up_write(&parent->lock);
      resdomain_put(rd);
      resdomain_uncharge_mem(rd, sizeof(struct pseudo_node));
      kfree(node);
      return nullptr;
    }
  }

  rb_link_node(&node->rb_node, p, link);
  rb_insert_color(&node->rb_node, &parent->children);

  up_write(&parent->lock);

  return node;
}

struct pseudo_node *pseudo_fs_create_dir(struct pseudo_fs_info *fs,
                                         struct pseudo_node *parent,
                                         const char *name) {
  return pseudo_fs_create_node(fs, parent, name, S_IFDIR | 0755, nullptr, nullptr);
}

struct pseudo_node *pseudo_fs_create_file(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          const struct file_operations *fops,
                                          void *private_data) {
  return pseudo_fs_create_node(fs, parent, name, S_IFREG | 0644, fops, private_data);
}

struct pseudo_node *pseudo_fs_create_link(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          const char *target) {
  struct pseudo_node *node = pseudo_fs_create_node(fs, parent, name, S_IFLNK | 0777, nullptr, nullptr);
  if (node) {
    /* Accounting for symlink target */
    size_t len = strlen(target) + 1;
    if (resdomain_charge_mem(node->rd, len, false) < 0) {
      pseudo_fs_remove_node(fs, node);
      return nullptr;
    }

    node->symlink_target = kstrdup(target);
    if (!node->symlink_target) {
      resdomain_uncharge_mem(node->rd, len);
      pseudo_fs_remove_node(fs, node);
      return nullptr;
    }
  }
  return node;
}

void pseudo_fs_remove_node(struct pseudo_fs_info *fs, struct pseudo_node *node) {
  if (!node || node == fs->root) return;

  /* Recursively remove children */
  down_write(&node->lock);

  struct pseudo_node *child;
  struct rb_node *n;

  while ((n = rb_first_postorder(&node->children))) {
    child = rb_entry(n, struct pseudo_node, rb_node);

    /* Unlock parent to remove child, then re-lock */
    up_write(&node->lock);
    pseudo_fs_remove_node(fs, child);
    down_write(&node->lock);
  }
  up_write(&node->lock);

  /* Unlink from parent */
  struct pseudo_node *parent = node->parent;
  if (parent) {
    down_write(&parent->lock);
    rb_erase(&node->rb_node, &parent->children);
    up_write(&parent->lock);
  }

  /* Cleanup associated inode if any */
  if (node->inode) {
    node->inode->i_fs_info = nullptr;
    iput(node->inode);
  }

  if (node->symlink_target) {
    resdomain_uncharge_mem(node->rd, strlen(node->symlink_target) + 1);
    kfree(node->symlink_target);
  }

  struct resdomain *rd = node->rd;
  kfree(node);

  if (rd) {
    resdomain_uncharge_mem(rd, sizeof(struct pseudo_node));
    resdomain_put(rd);
  }
}
