/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/tmpfs.c
 * @brief Advanced Temporary Filesystem (Linux-style tmpfs)
 * @copyright (C) 2026 assembler-0
 */

#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <aerosync/errno.h>
#include <aerosync/atomic.h>
#include <lib/string.h>
#include <aerosync/resdomain.h>
#include <lib/uaccess.h>
#include <linux/rbtree.h>
#include <aerosync/rw_semaphore.h>
#include <aerosync/mutex.h>
#include <aerosync/classes.h>
#include <lib/printk.h>

#define TMPFS_MAGIC 0x01021994

static atomic_t tmpfs_ino_counter = ATOMIC_INIT(1);

struct tmpfs_node {
  char name[64];
  vfs_mode_t mode;
  vfs_ino_t i_ino;
  struct inode *inode;        /* Active VFS inode (weak ref) */
  struct vm_object *obj;      /* Persistent Page Cache (UBC) */
  struct tmpfs_node *parent;
  struct rb_root children;    /* If directory */
  struct rb_node rb_node;     /* Entry in parent's tree */
  char *symlink_target;
  struct rw_semaphore lock;   /* Protects children */
  struct resdomain *rd;
  vfs_loff_t size;
  void *private_data;         /* For devices/special files */
  bool deleted;               /* True if unlinked */
};

struct tmpfs_sb_info {
  struct tmpfs_node *root;
};

static int tmpfs_cmp(const void *key, const struct rb_node *node) {
  const struct tmpfs_node *p = rb_entry(node, struct tmpfs_node, rb_node);
  return strcmp((const char *) key, p->name);
}

static struct inode_operations tmpfs_dir_inode_ops;
static struct inode_operations tmpfs_file_inode_ops;
static struct file_operations tmpfs_file_operations;
static struct file_operations tmpfs_dir_operations;
static struct inode_operations tmpfs_symlink_inode_ops;

static void tmpfs_free_node(struct tmpfs_node *node) {
  if (node->symlink_target) {
    resdomain_uncharge_mem(node->rd, strlen(node->symlink_target) + 1);
    kfree(node->symlink_target);
  }
  
  if (node->obj) {
    vm_object_put(node->obj);
  }

  struct resdomain *rd = node->rd;
  kfree(node);
  resdomain_uncharge_mem(rd, sizeof(struct tmpfs_node));
  resdomain_put(rd);
}

static void tmpfs_update_node(struct tmpfs_node *node, struct inode *inode) {
    if (!node || !inode) return;
    node->size = inode->i_size;
}

static ssize_t tmpfs_file_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
    ssize_t ret = filemap_write(file, buf, count, ppos);
    if (ret > 0) {
        tmpfs_update_node(file->f_inode->i_fs_info, file->f_inode);
    }
    return ret;
}

static struct file_operations tmpfs_file_operations = {
    .mmap = generic_file_mmap,
    .write = tmpfs_file_write,
    .read = filemap_read,
};

static struct inode *tmpfs_make_inode(struct super_block *sb, struct tmpfs_node *node) {
    struct inode *inode = new_inode(sb);
    if (!inode) return nullptr;

    inode->i_ino = node->i_ino;
    inode->i_mode = node->mode;
    inode->i_size = node->size;
    inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
    inode->i_fs_info = node;

    if (S_ISREG(node->mode)) {
        inode->i_op = &tmpfs_file_inode_ops;
        inode->i_fop = &tmpfs_file_operations;
        
        if (!node->obj) {
            node->obj = vm_object_alloc(VM_OBJECT_VNODE);
            if (node->obj) {
                node->obj->vnode = inode; 
                node->obj->size = node->size;
                node->obj->flags |= VM_OBJECT_SWAP_BACKED;
                node->obj->rd = node->rd;
                resdomain_get(node->rd);
                extern const struct vm_object_operations vnode_ubc_ops;
                node->obj->ops = &vnode_ubc_ops;
            }
        }
        
        if (node->obj) {
            inode->i_ubc = node->obj;
            vm_object_get(node->obj);
            inode->i_ubc->vnode = inode;
        }
    } else if (S_ISDIR(node->mode)) {
        inode->i_op = &tmpfs_dir_inode_ops;
        inode->i_fop = &tmpfs_dir_operations;
    } else if (S_ISLNK(node->mode)) {
        inode->i_op = &tmpfs_symlink_inode_ops;
    } else {
        init_special_inode(inode, node->mode, (dev_t)(uintptr_t)node->private_data);
    }

    node->inode = inode;
    return inode;
}

static struct tmpfs_node *tmpfs_alloc_node(struct resdomain *rd, const char *name, vfs_mode_t mode) {
  if (resdomain_charge_mem(rd, sizeof(struct tmpfs_node), false) < 0)
    return nullptr;

  struct tmpfs_node *node = kzalloc(sizeof(struct tmpfs_node));
  if (!node) {
    resdomain_uncharge_mem(rd, sizeof(struct tmpfs_node));
    return nullptr;
  }

  strncpy(node->name, name, 63);
  node->mode = mode;
  node->i_ino = atomic_inc_return(&tmpfs_ino_counter);
  node->children = RB_ROOT;
  rwsem_init(&node->lock);
  node->rd = rd;
  resdomain_get(rd);

  return node;
}

static int tmpfs_iterate(struct file *file, struct dir_context *ctx) {
  struct tmpfs_node *parent = file->f_inode->i_fs_info;
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

  down_read(&parent->lock);
  struct rb_node *n;
  vfs_loff_t i = 2;

  for (n = rb_first(&parent->children); n; n = rb_next(n)) {
    if (i >= ctx->pos) {
      struct tmpfs_node *node = rb_entry(n, struct tmpfs_node, rb_node);
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

static struct dentry *tmpfs_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
  (void) flags;
  struct tmpfs_node *parent = dir->i_fs_info;
  if (!parent) return nullptr;

  down_read(&parent->lock);
  struct rb_node *match = rb_find((const char *) dentry->d_name.name, &parent->children, tmpfs_cmp);
  if (match) {
    struct tmpfs_node *node = rb_entry(match, struct tmpfs_node, rb_node);
    struct inode *inode = node->inode;
    if (inode) {
      iget(inode);
    } else {
      inode = tmpfs_make_inode(dir->i_sb, node);
    }
    dentry->d_inode = inode;
    up_read(&parent->lock);
    return dentry;
  }
  up_read(&parent->lock);
  return nullptr;
}

static int tmpfs_do_mknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
  struct tmpfs_node *parent = dir->i_fs_info;
  struct resdomain *rd = current ? current->rd : &root_resdomain;

  struct tmpfs_node *node = tmpfs_alloc_node(rd, (const char *) dentry->d_name.name, mode);
  if (!node) return -ENOMEM;

  node->parent = parent;
  node->private_data = (void *)(uintptr_t)dev;

  down_write(&parent->lock);
  struct rb_node **link = &parent->children.rb_node;
  struct rb_node *p = nullptr;
  while (*link) {
    p = *link;
    struct tmpfs_node *entry = rb_entry(p, struct tmpfs_node, rb_node);
    int cmp = strcmp(node->name, entry->name);
    if (cmp < 0) link = &p->rb_left;
    else if (cmp > 0) link = &p->rb_right;
    else {
      up_write(&parent->lock);
      tmpfs_free_node(node);
      return -EEXIST;
    }
  }
  rb_link_node(&node->rb_node, p, link);
  rb_insert_color(&node->rb_node, &parent->children);
  up_write(&parent->lock);

  struct inode *inode = tmpfs_make_inode(dir->i_sb, node);
  if (!inode) {
    down_write(&parent->lock);
    rb_erase(&node->rb_node, &parent->children);
    up_write(&parent->lock);
    tmpfs_free_node(node);
    return -ENOMEM;
  }

  dentry->d_inode = inode;
  dir->i_mtime = dir->i_ctime = current_time(dir);
  return 0;
}

static int tmpfs_mknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev) {
    return tmpfs_do_mknod(dir, dentry, mode, dev);
}

static int tmpfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_do_mknod(dir, dentry, mode | S_IFDIR, 0);
}

static int tmpfs_create(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  return tmpfs_do_mknod(dir, dentry, mode | S_IFREG, 0);
}

static int tmpfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname) {
  int ret = tmpfs_do_mknod(dir, dentry, S_IFLNK | 0777, 0);
  if (ret == 0) {
    struct tmpfs_node *node = dentry->d_inode->i_fs_info;
    size_t len = strlen(oldname) + 1;
    if (resdomain_charge_mem(node->rd, len, false) < 0) {
      return -ENOMEM;
    }
    node->symlink_target = kstrdup(oldname);
  }
  return ret;
}

static ssize_t tmpfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz) {
  struct tmpfs_node *node = dentry->d_inode->i_fs_info;
  if (!node || !node->symlink_target) return -EINVAL;
  size_t len = strlen(node->symlink_target);
  if (len > bufsiz) len = bufsiz;

  if ((uintptr_t)buf >= vmm_get_max_user_address()) {
      memcpy(buf, node->symlink_target, len);
  } else {
      if (copy_to_user(buf, node->symlink_target, len)) return -EFAULT;
  }
  return (ssize_t) len;
}

static const char *tmpfs_follow_link(struct dentry *dentry, void **cookie) {
  struct tmpfs_node *node = dentry->d_inode->i_fs_info;
  if (!node || !node->symlink_target) return ERR_PTR(-EINVAL);
  *cookie = nullptr;
  return node->symlink_target;
}

static int tmpfs_unlink(struct inode *dir, struct dentry *dentry) {
  struct tmpfs_node *parent = dir->i_fs_info;
  struct tmpfs_node *node = dentry->d_inode->i_fs_info;
  if (!parent || !node) return -EINVAL;

  down_write(&parent->lock);
  rb_erase(&node->rb_node, &parent->children);
  node->deleted = true;
  up_write(&parent->lock);

  node->inode = nullptr;
  dir->i_mtime = dir->i_ctime = current_time(dir);
  return 0;
}

static int tmpfs_rmdir(struct inode *dir, struct dentry *dentry) {
  struct tmpfs_node *node = dentry->d_inode->i_fs_info;
  if (!node) return -EINVAL;

  down_read(&node->lock);
  if (rb_first(&node->children)) {
    up_read(&node->lock);
    return -ENOTEMPTY;
  }
  up_read(&node->lock);

  return tmpfs_unlink(dir, dentry);
}

static struct inode_operations tmpfs_dir_inode_ops = {
  .lookup = tmpfs_lookup,
  .mkdir = tmpfs_mkdir,
  .create = tmpfs_create,
  .mknod = tmpfs_mknod,
  .symlink = tmpfs_symlink,
  .unlink = tmpfs_unlink,
  .rmdir = tmpfs_rmdir,
};

static struct file_operations tmpfs_dir_operations = {
  .iterate = tmpfs_iterate,
};

static struct inode_operations tmpfs_file_inode_ops = {
  .getattr = nullptr,
};

static struct inode_operations tmpfs_symlink_inode_ops = {
  .readlink = tmpfs_readlink,
  .follow_link = tmpfs_follow_link,
};

static void tmpfs_destroy_inode(struct inode *inode) {
  struct tmpfs_node *node = inode->i_fs_info;
  if (node) {
    tmpfs_update_node(node, inode);
    node->inode = nullptr;
    if (node->deleted) {
        tmpfs_free_node(node);
    }
  }
  kfree(inode);
}

static struct super_operations tmpfs_ops = {
  .destroy_inode = tmpfs_destroy_inode,
};

int tmpfs_fill_super(struct super_block *sb, void *data) {
  (void) data;
  struct tmpfs_sb_info *sbi = kzalloc(sizeof(struct tmpfs_sb_info));
  if (!sbi) return -ENOMEM;
  sb->s_fs_info = sbi;
  sb->s_maxbytes = 0x7fffffffffffffffULL;
  sb->s_blocksize = PAGE_SIZE;
  sb->s_magic = TMPFS_MAGIC;
  sb->s_op = &tmpfs_ops;

  struct resdomain *rd = &root_resdomain;
  sbi->root = tmpfs_alloc_node(rd, "/", S_IFDIR | 0755);
  if (!sbi->root) {
    kfree(sbi);
    return -ENOMEM;
  }

  struct inode *inode = tmpfs_make_inode(sb, sbi->root);
  if (!inode) {
    tmpfs_free_node(sbi->root);
    kfree(sbi);
    return -ENOMEM;
  }

  struct qstr root_name = {.name = (const unsigned char *) "/", .len = 1};
  extern struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);
  sb->s_root = d_alloc_pseudo(sb, &root_name);
  if (!sb->s_root) {
    iput(inode);
    tmpfs_free_node(sbi->root);
    kfree(sbi);
    return -ENOMEM;
  }
  sb->s_root->d_inode = inode;
  return 0;
}

static int tmpfs_mount(struct file_system_type *fs_type, const char *dev_name, const char *dir_name,
                       unsigned long flags, void *data) {
  (void) dev_name; (void) dir_name; (void) flags; (void) fs_type;
  struct super_block *sb = kzalloc(sizeof(struct super_block));
  if (!sb) return -ENOMEM;

  int ret = tmpfs_fill_super(sb, data);
  if (ret) {
    kfree(sb);
    return ret;
  }

  extern struct list_head super_blocks;
  extern struct mutex sb_mutex;
  mutex_lock(&sb_mutex);
  list_add_tail(&sb->sb_list, &super_blocks);
  mutex_unlock(&sb_mutex);
  return 0;
}

static void tmpfs_kill_sb(struct super_block *sb) {
  struct tmpfs_sb_info *sbi = sb->s_fs_info;
  if (sbi) {
    kfree(sbi);
  }
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

/* --- Kernel Internal API for devtmpfs --- */

int tmpfs_create_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name, vfs_mode_t mode, dev_t dev) {
    if (!parent) {
        struct tmpfs_sb_info *sbi = sb->s_fs_info;
        parent = sbi->root;
    }

    struct dentry dentry;
    dentry.d_name.name = (const unsigned char *)name;
    dentry.d_name.len = strlen(name);
    return tmpfs_do_mknod(parent->inode, &dentry, mode, dev);
}

struct tmpfs_node *tmpfs_mkdir_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name, vfs_mode_t mode) {
    if (!parent) {
        struct tmpfs_sb_info *sbi = sb->s_fs_info;
        parent = sbi->root;
    }

    struct dentry dentry;
    dentry.d_name.name = (const unsigned char *)name;
    dentry.d_name.len = strlen(name);
    int ret = tmpfs_do_mknod(parent->inode, &dentry, mode | S_IFDIR, 0);
    if (ret != 0 && ret != -EEXIST) return nullptr;

    /* Find the node we just created (or that already existed) */
    down_read(&parent->lock);
    struct rb_node *match = rb_find(name, &parent->children, tmpfs_cmp);
    up_read(&parent->lock);

    if (!match) return nullptr;
    return rb_entry(match, struct tmpfs_node, rb_node);
}

int tmpfs_remove_kern(struct super_block *sb, struct tmpfs_node *parent, const char *name) {
    if (!parent) {
        struct tmpfs_sb_info *sbi = sb->s_fs_info;
        parent = sbi->root;
    }

    down_write(&parent->lock);
    struct rb_node *match = rb_find(name, &parent->children, tmpfs_cmp);
    if (!match) {
        up_write(&parent->lock);
        return -ENOENT;
    }
    struct tmpfs_node *node = rb_entry(match, struct tmpfs_node, rb_node);
    rb_erase(&node->rb_node, &parent->children);
    node->deleted = true;
    if (node->inode) {
        node->inode->i_fs_info = nullptr;
    }
    up_write(&parent->lock);
    
    if (!node->inode) {
        tmpfs_free_node(node);
    }
    return 0;
}
