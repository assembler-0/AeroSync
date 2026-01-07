#pragma once

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <fs/vfs.h>
#include <kernel/atomic.h>

struct file;
struct dentry;
struct inode;

#define NR_OPEN_DEFAULT 64

struct fdtable {
    unsigned int max_fds;
    struct file **fd;      /* current fd array */
    unsigned long *open_fds;
    unsigned long *close_on_exec;
};

struct files_struct {
    atomic_t count;
    spinlock_t file_lock;
    int next_fd;
    struct fdtable fdtab;
    
    /* initial fd set */
    struct file *fd_array[NR_OPEN_DEFAULT];
    unsigned long open_fds_init[NR_OPEN_DEFAULT / (8 * sizeof(long))];
    unsigned long close_on_exec_init[NR_OPEN_DEFAULT / (8 * sizeof(long))];
};

struct file *fget(unsigned int fd);
void fput(struct file *file);
int fd_install(unsigned int fd, struct file *file);
int get_unused_fd_flags(unsigned int flags);
void put_unused_fd(unsigned int fd);

/* VFS core functions */
struct file *vfs_open(const char *path, int flags, int mode);
ssize_t vfs_read(struct file *file, char *buf, size_t count, vfs_loff_t *pos);
ssize_t vfs_write(struct file *file, const char *buf, size_t count, vfs_loff_t *pos);
int vfs_close(struct file *file);
vfs_loff_t vfs_llseek(struct file *file, vfs_loff_t offset, int whence);

/* Inode and dentry helpers */
struct dentry *vfs_path_lookup(const char *path, unsigned int flags);
struct inode *new_inode(struct super_block *sb);
void iput(struct inode *inode);
void dput(struct dentry *dentry);

extern struct files_struct init_files;
