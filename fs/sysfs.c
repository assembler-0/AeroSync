/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/sysfs.c
 * @brief System Filesystem (Kernel Object & ActiveControl View)
 * @copyright (C) 2026 assembler-0
 */

#ifdef CONFIG_SYSFS

#include <fs/pseudo_fs.h>
#include <fs/sysfs.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/attribute.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <lib/printk.h>
#include <aerosync/errno.h>
#include <mm/slub.h>

static struct pseudo_fs_info sysfs_info = {
  .name = "sysfs",
};

/* Base directories as defined in AeroQ HFS */
static struct pseudo_node *sys_sched_node;
static struct pseudo_node *sys_mm_node;
static struct pseudo_node *sys_perf_node;
static struct pseudo_node *sys_security_node;
static struct pseudo_node *sys_misc_node;
static struct pseudo_node *sys_actl_node;
static struct pseudo_node *sys_devices_node;

/* --- Attribute File Operations --- */

struct sysfs_attr_context {
    struct device *dev;
    struct device_attribute *attr;
};

static ssize_t sysfs_attr_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
    struct pseudo_node *node = file->f_inode->i_fs_info;
    if (!node) return -EIO;
    struct sysfs_attr_context *ctx = node->private_data;
    if (!ctx || !ctx->attr->show) return -EIO;

    char *kbuf = kmalloc(PAGE_SIZE);
    if (!kbuf) return -ENOMEM;

    ssize_t len = ctx->attr->show(ctx->dev, ctx->attr, kbuf);
    if (len < 0) {
        kfree(kbuf);
        return len;
    }

    ssize_t ret = simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
    kfree(kbuf);
    return ret;
}

static ssize_t sysfs_attr_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
    (void) ppos;
    struct pseudo_node *node = file->f_inode->i_fs_info;
    if (!node) return -EIO;
    struct sysfs_attr_context *ctx = node->private_data;
    if (!ctx || !ctx->attr->store) return -EIO;

    char *kbuf = kmalloc(PAGE_SIZE);
    if (!kbuf) return -ENOMEM;

    size_t copy_len = count > PAGE_SIZE - 1 ? PAGE_SIZE - 1 : count;
    
    if (file->f_mode & FMODE_KERNEL) {
        memcpy(kbuf, buf, copy_len);
    } else {
        if (copy_from_user(kbuf, buf, copy_len)) {
            kfree(kbuf);
            return -EFAULT;
        }
    }
    kbuf[copy_len] = 0;

    ssize_t ret = ctx->attr->store(ctx->dev, ctx->attr, kbuf, copy_len);
    kfree(kbuf);
    return ret;
}

static const struct file_operations sysfs_attr_fops = {
    .read = sysfs_attr_read,
    .write = sysfs_attr_write,
};

void sysfs_init(void) {
  pseudo_fs_register(&sysfs_info);

  sys_sched_node    = pseudo_fs_create_dir(&sysfs_info, nullptr, "sched");
  sys_mm_node       = pseudo_fs_create_dir(&sysfs_info, nullptr, "mm");
  sys_perf_node     = pseudo_fs_create_dir(&sysfs_info, nullptr, "perf");
  sys_security_node = pseudo_fs_create_dir(&sysfs_info, nullptr, "security");
  sys_misc_node     = pseudo_fs_create_dir(&sysfs_info, nullptr, "misc");
  sys_devices_node  = pseudo_fs_create_dir(&sysfs_info, nullptr, "devices");
  
  /* actl/ - ActiveControl (Direct kernel control) */
  sys_actl_node     = pseudo_fs_create_dir(&sysfs_info, nullptr, "actl");
  if (sys_actl_node) {
      pseudo_fs_create_dir(&sysfs_info, sys_actl_node, "mm");
      pseudo_fs_create_dir(&sysfs_info, sys_actl_node, "sched");
      pseudo_fs_create_dir(&sysfs_info, sys_actl_node, "perf");
      pseudo_fs_create_dir(&sysfs_info, sys_actl_node, "security");
      pseudo_fs_create_dir(&sysfs_info, sys_actl_node, "trace");
  }
}

static struct pseudo_node *sysfs_get_parent(const char *parent_name) {
    if (!parent_name) return nullptr;
    if (strcmp(parent_name, "sched") == 0) return sys_sched_node;
    if (strcmp(parent_name, "mm") == 0) return sys_mm_node;
    if (strcmp(parent_name, "perf") == 0) return sys_perf_node;
    if (strcmp(parent_name, "security") == 0) return sys_security_node;
    if (strcmp(parent_name, "misc") == 0) return sys_misc_node;
    if (strcmp(parent_name, "devices") == 0) return sys_devices_node;
    if (strcmp(parent_name, "actl") == 0) return sys_actl_node;
    
    if (strncmp(parent_name, "actl/", 5) == 0) {
        return pseudo_fs_find_node(sys_actl_node, parent_name + 5);
    }
    
    return nullptr;
}

int sysfs_create_dir_kern(const char *name, const char *parent_name) {
    struct pseudo_node *parent = sysfs_get_parent(parent_name);
    struct pseudo_node *node = pseudo_fs_create_dir(&sysfs_info, parent, name);
    return node ? 0 : -ENOMEM;
}

int sysfs_create_file_kern(const char *name, const struct file_operations *fops, void *private_data, const char *parent_name) {
    struct pseudo_node *parent = sysfs_get_parent(parent_name);
    struct pseudo_node *node = pseudo_fs_create_file(&sysfs_info, parent, name, fops, private_data);
    return node ? 0 : -ENOMEM;
}

/* 
 * Create sysfs attributes for a device.
 */
static void sysfs_destroy_attr(struct pseudo_node *node) {
    if (node->private_data) {
        kfree(node->private_data);
        node->private_data = nullptr;
    }
}

static int sysfs_create_attr(struct device *dev, struct pseudo_node *dir, struct device_attribute *attr) {
    struct sysfs_attr_context *ctx = kmalloc(sizeof(*ctx));
    if (!ctx) return -ENOMEM;
    ctx->dev = dev;
    ctx->attr = attr;

    struct pseudo_node *node = pseudo_fs_create_file(&sysfs_info, dir, attr->attr.name, &sysfs_attr_fops, ctx);
    if (!node) {
        kfree(ctx);
        return -ENOMEM;
    }
    node->destroy_node = sysfs_destroy_attr;
    return 0;
}

int sysfs_register_device(struct device *dev) {
    if (!dev || !dev->name || !sys_devices_node) return -EINVAL;

    /* Create device directory in /runtime/sys/devices/<name> */
    struct pseudo_node *dev_dir = pseudo_fs_create_dir(&sysfs_info, sys_devices_node, dev->name);
    if (!dev_dir) return -ENOMEM;

    /* Populate attributes from groups */
    if (dev->groups) {
        for (int i = 0; dev->groups[i]; i++) {
            const struct attribute_group *grp = dev->groups[i];
            struct pseudo_node *parent = dev_dir;
            
            if (grp->name) {
                parent = pseudo_fs_create_dir(&sysfs_info, dev_dir, grp->name);
                if (!parent) continue;
            }

            for (int j = 0; grp->attrs[j]; j++) {
                struct device_attribute *dattr = (struct device_attribute *) grp->attrs[j];
                sysfs_create_attr(dev, parent, dattr);
            }
        }
    }

    return 0;
}

void sysfs_unregister_device(struct device *dev) {
    if (!dev || !dev->name || !sys_devices_node) return;
    struct pseudo_node *dev_dir = pseudo_fs_find_node(sys_devices_node, dev->name);
    if (dev_dir) {
        /* 
         * Note: pseudo_fs_remove_node should recursively remove children
         * and we'll need to free the sysfs_attr_context in a callback if we had one.
         * For now, we assume simple teardown.
         */
        pseudo_fs_remove_node(&sysfs_info, dev_dir);
    }
}

#endif
