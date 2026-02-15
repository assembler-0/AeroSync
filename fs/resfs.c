/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/resfs.c
 * @brief Advanced Resource Domain Filesystem (ResFS)
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/resdomain.h>
#include <fs/pseudo_fs.h>
#include <aerosync/errno.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/string.h>
#include <mm/slub.h>
#include <aerosync/sched/process.h>
#include <lib/uaccess.h>

struct pseudo_fs_info resfs_info = {
  .name = "resfs",
};

extern struct task_struct *find_task_by_pid(pid_t pid);

void resfs_init_inode(struct inode *inode, struct pseudo_node *pnode) {
  inode->i_mode = pnode->mode;
  inode->i_fs_info = pnode->private_data;
  if (S_ISDIR(pnode->mode)) {
    extern struct inode_operations resfs_dir_iop;
    inode->i_op = &resfs_dir_iop;
  }
}

/* --- Core Control Files --- */

static ssize_t resfs_controllers_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  char kbuf[256];
  int len = 0;
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    if (rd->child_subsys_mask & (1 << i)) {
      len += snprintf(kbuf + len, sizeof(kbuf) - len, "%s ", rd_subsys_list[i]->name);
    }
  }
  if (len > 0) {
    kbuf[len - 1] = '\n';
  } else {
    len = snprintf(kbuf, sizeof(kbuf), "\n");
  }
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations resfs_controllers_fops = {
  .read = resfs_controllers_read,
};

static ssize_t resfs_subtree_control_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  struct resdomain *rd = file->f_inode->i_fs_info;
  char kbuf[256];
  int len = 0;
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    if (rd->subtree_control & (1 << i)) {
      len += snprintf(kbuf + len, sizeof(kbuf) - len, "+%s ", rd_subsys_list[i]->name);
    }
  }
  if (len > 0) {
    kbuf[len - 1] = '\n';
  } else {
    len = snprintf(kbuf, sizeof(kbuf), "\n");
  }
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t resfs_subtree_control_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  char kbuf[128];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;

  /* Simplified parser: +name or -name */
  char *p = kbuf;
  while (*p) {
    while (*p == ' ' || *p == '\n') p++;
    if (!*p) break;

    bool enable = true;
    if (*p == '+') { enable = true; p++; }
    else if (*p == '-') { enable = false; p++; }

    char *name = p;
    while (*p && *p != ' ' && *p != '\n') p++;
    char saved = *p;
    *p = 0;

    int id = -1;
    for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
      if (strcmp(name, rd_subsys_list[i]->name) == 0) {
        id = i;
        break;
      }
    }

    if (id != -1) {
      spinlock_lock(&rd->lock);
      if (enable) {
        if (rd->child_subsys_mask & (1 << id))
          rd->subtree_control |= (1 << id);
      } else {
        rd->subtree_control &= ~(1 << id);
      }
      spinlock_unlock(&rd->lock);
    }

    *p = saved;
  }

  return (ssize_t) count;
}

static const struct file_operations resfs_subtree_control_fops = {
  .read = resfs_subtree_control_read,
  .write = resfs_subtree_control_write,
};

static ssize_t resfs_procs_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  /* Iterate all tasks and list those in this RD */
  (void) file; (void) buf; (void) count; (void) ppos;
  return 0; /* Placeholder for full iteration */
}

static ssize_t resfs_procs_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct resdomain *rd = file->f_inode->i_fs_info;
  char kbuf[16];
  if (count >= sizeof(kbuf)) return -EINVAL;
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;

  int pid;
  if (kstrtos(kbuf, 10, &pid)) return -EINVAL;

  struct task_struct *task = find_task_by_pid((pid_t) pid);
  if (!task) return -ESRCH;

  int ret = resdomain_attach_task(rd, task);
  return ret < 0 ? (ssize_t) ret : (ssize_t) count;
}

static const struct file_operations resfs_procs_fops = {
  .read = resfs_procs_read,
  .write = resfs_procs_write,
};

/* --- Management --- */

static void resfs_populate_dir(struct pseudo_node *dir, struct resdomain *rd) {
  struct pseudo_node *node;

  /* Standard cgroup v2-style control files */
  node = pseudo_fs_create_file(&resfs_info, dir, "rd.controllers", &resfs_controllers_fops, rd);
  if (node) node->init_inode = resfs_init_inode;

  node = pseudo_fs_create_file(&resfs_info, dir, "rd.subtree_control", &resfs_subtree_control_fops, rd);
  if (node) node->init_inode = resfs_init_inode;

  node = pseudo_fs_create_file(&resfs_info, dir, "rd.procs", &resfs_procs_fops, rd);
  if (node) node->init_inode = resfs_init_inode;

  /* Subsystem-specific files */
  for (int i = 0; i < RD_SUBSYS_COUNT; i++) {
    if (rd->subsys[i] && rd_subsys_list[i]->populate) {
      rd_subsys_list[i]->populate(rd, dir);
    }
  }
}

static int resfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode);

struct inode_operations resfs_dir_iop = {
  .lookup = simple_lookup,
  .mkdir = resfs_mkdir,
  .rmdir = simple_rmdir,
};

static int resfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode) {
  (void) mode;
  struct resdomain *parent_rd = dir->i_fs_info;
  if (!parent_rd) return -EPERM;

  struct resdomain *new_rd = resdomain_create(parent_rd, (const char *) dentry->d_name.name);
  if (!new_rd) return -ENOMEM;

  return 0;
}

void resfs_bind_domain(struct resdomain *rd) {
  if (!resfs_info.root) return;

  struct pseudo_node *parent_node = resfs_info.root;
  if (rd->parent) {
    parent_node = rd->parent->private_data;
  }

  if (!parent_node) return;

  struct pseudo_node *node = pseudo_fs_create_node(&resfs_info, parent_node, rd->name, S_IFDIR | 0755, nullptr, rd);
  if (node) {
    rd->private_data = node;
    node->init_inode = resfs_init_inode;
    resfs_populate_dir(node, rd);
  }
}

void resfs_init(void) {
  pseudo_fs_register(&resfs_info);

  root_resdomain.private_data = resfs_info.root;
  resfs_populate_dir(resfs_info.root, &root_resdomain);
}
