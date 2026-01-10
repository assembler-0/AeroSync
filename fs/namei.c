#include <fs/vfs.h>
#include <lib/string.h>
#include <mm/slab.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

struct dentry *root_dentry = NULL;

struct dentry *vfs_path_lookup(const char *path, unsigned int flags) {
    if (!path || !root_dentry) return NULL;

    printk(KERN_DEBUG VFS_CLASS "vfs_path_lookup: \"%s\"\n", path);

    if (strcmp(path, "/") == 0) {
        return root_dentry;
    }

    // Very simple lookup: only supports root for now
    // If it starts with / and then has something else, we don't support it yet
    if (path[0] == '/' && path[1] == '\0') {
        return root_dentry;
    }
    
    // In a real implementation, this would walk the path components
    // and use i_op->lookup for each component.
    
    return NULL;
}

struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name) {
    struct dentry *dentry = kzalloc(sizeof(struct dentry));
    if (!dentry) return NULL;

    dentry->d_name.name = (unsigned char *)kstrdup((const char *)name->name);
    dentry->d_name.len = name->len;
    spinlock_init(&dentry->d_lock);
    INIT_LIST_HEAD(&dentry->d_subdirs);
    INIT_LIST_HEAD(&dentry->d_child);
    
    return dentry;
}