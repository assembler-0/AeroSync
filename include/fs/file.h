#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <fs/vfs.h>
#include <aerosync/atomic.h>

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

int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_fcntl(int fd, unsigned int cmd, unsigned long arg);

/* Inode and dentry helpers */
struct inode *new_inode(struct super_block *sb);
void iput(struct inode *inode);
void iget(struct inode *inode);
void dput(struct dentry *dentry);
struct dentry *dget(struct dentry *dentry);
struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);

/* fs/filemap.c */
ssize_t filemap_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos);
ssize_t filemap_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos);

extern struct files_struct init_files;
