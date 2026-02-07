/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/pseudo_fs.c
 * @brief Generic Pseudo-Filesystem Library (APS)
 * @copyright (C) 2026 assembler-0
 */

#include <fs/pseudo_fs.h>
#include <fs/vfs.h>
#include <mm/slub.h>
#include <lib/string.h>
#include <aerosync/errno.h>

/* --- Common Inode Operations --- */

static struct dentry *pseudo_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags) {
    (void)flags;
    struct pseudo_node *parent = dir->i_fs_info;
    struct pseudo_node *node;

    if (!parent) return nullptr;

    /* Search children of the parent node */
    list_for_each_entry(node, &parent->children, sibling) {
        if (strcmp((const char *)dentry->d_name.name, node->name) == 0) {
            struct inode *inode = new_inode(dir->i_sb);
            if (!inode) return nullptr;

            if (node->init_inode) {
                node->init_inode(inode, node);
            } else {
                inode->i_mode = node->mode;
                inode->i_op = node->iop ? node->iop : dir->i_op;
                inode->i_fop = node->fop;
            }
            
            inode->i_fs_info = node;
            inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);
            
            dentry->d_inode = inode;
            node->inode = inode;
            return dentry;
        }
    }

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

    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_op = &pseudo_dir_iop;
    root_inode->i_fs_info = info->root;
    root_inode->i_atime = root_inode->i_mtime = root_inode->i_ctime = current_time(root_inode);

    struct qstr root_name = {.name = (const unsigned char *)"/", .len = 1};
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
    (void)dev_name; (void)dir_name; (void)flags;
    struct pseudo_fs_info *info = (struct pseudo_fs_info *)fs_type->name; // Hacky: use name as pointer
    
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

int pseudo_fs_register(struct pseudo_fs_info *info) {
    if (!info || !info->name) return -EINVAL;

    info->root = kzalloc(sizeof(struct pseudo_node));
    if (!info->root) return -ENOMEM;

    strncpy(info->root->name, "/", 64);
    info->root->mode = S_IFDIR | 0755;
    INIT_LIST_HEAD(&info->root->children);
    INIT_LIST_HEAD(&info->root->sibling);

    info->fs_type.name = info->name;
    info->fs_type.mount = pseudo_mount;
    info->fs_type.kill_sb = nullptr; /* TODO */

    return register_filesystem(&info->fs_type);
}

struct pseudo_node *pseudo_fs_create_node(struct pseudo_fs_info *fs,
                                          struct pseudo_node *parent,
                                          const char *name,
                                          vfs_mode_t mode,
                                          const struct file_operations *fops,
                                          void *private_data) {
    struct pseudo_node *node = kzalloc(sizeof(struct pseudo_node));
    if (!node) return nullptr;

    if (!parent) parent = fs->root;

    strncpy(node->name, name, 64);
    node->mode = mode;
    node->fop = fops;
    node->private_data = private_data;
    node->parent = parent;
    INIT_LIST_HEAD(&node->children);
    
    list_add_tail(&node->sibling, &parent->children);
    return node;
}
