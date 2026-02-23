#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/atomic.h>
#include <linux/list.h>
#include <aerosync/wait.h>

// Forward declarations for VFS structures
struct super_block;
struct inode;
struct dentry;
struct file;
struct resdomain;

// VFS specific types
typedef uint64_t vfs_ino_t;      // Inode number type
typedef uint64_t vfs_off_t;      // Offset type (for file positions)
typedef uint32_t vfs_mode_t;     // File mode/permissions type
typedef uint32_t vfs_nlink_t;    // Number of links type
typedef uint64_t vfs_loff_t;     // Large file offset type
typedef uint32_t uid_t;          // User ID
typedef uint32_t gid_t;          // Group ID

struct timespec {
    time_t tv_sec;  // Seconds
    long tv_nsec;   // Nanoseconds
};

// File types (for vfs_mode_t)
#define S_IFMT   0xF000  // Mask for file type
#define S_IFSOCK 0xC000  // Socket
#define S_IFLNK  0xA000  // Symbolic link
#define S_IFREG  0x8000  // Regular file
#define S_IFBLK  0x6000  // Block device
#define S_IFDIR  0x4000  // Directory
#define S_IFCHR  0x2000  // Character device
#define S_IFIFO  0x1000  // FIFO

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

// File permissions (for vfs_mode_t)
#define S_IRWXU  00700  // User read, write, execute
#define S_IRUSR  00400  // User read
#define S_IWUSR  00200  // User write
#define S_IXUSR  00100  // User execute
#define S_IRWXG  00070  // Group read, write, execute
#define S_IRGRP  00040  // Group read
#define S_IWGRP  00020  // Group write
#define S_IXGRP  00010  // Group execute
#define S_IRWXO  00007  // Others read, write, execute
#define S_IROTH  00004  // Others read
#define S_IWOTH  00002  // Others write
#define S_IXOTH  00001  // Others execute

// File access modes (for open flags)
#define O_ACCMODE  0003  // Mask for access modes
#define O_RDONLY   0000  // Read only
#define O_WRONLY   0001  // Write only
#define O_RDWR     0002  // Read and write

#define O_CREAT    00100 // Create file if it does not exist
#define O_EXCL     00200 // Exclusive use flag
#define O_NOCTTY   00400 // Do not assign controlling terminal
#define O_TRUNC    01000 // Truncate file to zero length
#define O_APPEND   02000 // Set append mode
#define O_NONBLOCK 04000 // Non-blocking I/O
#define O_DSYNC    010000 // Write with data integrity sync
#define O_FSYNC    020000 // Write with file integrity sync (implies O_DSYNC)
#define O_ASYNC    0200000 // Asynchronous I/O
#define O_CLOEXEC  02000000 // Set close-on-exec

#define FMODE_READ   0x1
#define FMODE_WRITE  0x2
#define FMODE_KERNEL 0x1000

#define LOOKUP_PARENT 0x0001
#define LOOKUP_FOLLOW 0x0002

// VFS object operations forward declarations
struct file_operations;
struct inode_operations;
struct super_operations;

// struct qstr: Quick string for dentry names (optimization for hash lookups)
// This structure holds a pointer to the string and its length,
// potentially a hash for quick comparisons.
struct qstr {
    const unsigned char *name;
    uint32_t            len;
    uint32_t            hash; // Precomputed hash for faster lookups
};

// struct super_block: Represents a mounted filesystem
struct super_block {
    struct list_head    sb_list;          // List of all superblocks
    spinlock_t          sb_lock;          // Protects superblock data
    struct dentry       *s_root;          // Pointer to root dentry of filesystem
    const struct super_operations *s_op;  // Superblock operations
    void                *s_fs_info;       // Filesystem private data
    uint32_t            s_magic;          // Filesystem magic number
    struct resdomain    *s_resdomain;      // Associated resource domain
    dev_t               s_dev;            // Device identifier
    uint32_t            s_blocksize;      // Block size in bytes
    vfs_loff_t          s_maxbytes;       // Maximum file size
    // ... more fields like device, flags, etc.
};

// struct inode: Represents a file or directory
struct inode {
    struct list_head    i_list;           // All inodes in use list (global)
    struct list_head    i_dentry;         // List of dentries referencing this inode
    spinlock_t          i_lock;           // Protects inode data
    atomic_t            i_count;          // Reference count
    vfs_ino_t           i_ino;            // Inode number
    vfs_mode_t          i_mode;           // File type and permissions
    vfs_nlink_t         i_nlink;          // Number of hard links
    uid_t               i_uid;            // User ID of owner
    gid_t               i_gid;            // Group ID of owner
    vfs_loff_t          i_size;           // File size in bytes
    struct timespec     i_atime;          // Last access time
    struct timespec     i_mtime;          // Last modification time
    struct timespec     i_ctime;          // Last status change time
    struct super_block  *i_sb;            // Pointer to superblock
    struct vm_object    *i_ubc;           // Unified Buffer Cache / Mapping
    dev_t               i_rdev;           // Device number (if special file)
    const struct inode_operations *i_op;  // Inode operations
    const struct file_operations  *i_fop; // Default file operations
    void                *i_fs_info;       // Filesystem private data
    struct wait_queue_head i_wait;        // Waiters for I/O on this inode
    // ... more fields for devices, etc.
};

// struct dentry: Represents a directory entry (filename in a directory)
struct dentry {
    struct list_head    d_hash;           // Hash list for dentry cache
    struct list_head    d_lru;            // LRU list for dentry cache (if implementing LRU)
    struct list_head    d_child;          // List of children in parent dentry
    struct list_head    d_subdirs;        // List of subdirectories (if this is a directory dentry)
    struct list_head    i_list;           // Node for inode->i_dentry list
    struct dentry       *d_parent;        // Parent dentry
    struct qstr         d_name;           // Name of this dentry
    struct inode        *d_inode;         // Inode corresponding to this dentry (or nullptr if negative dentry)
    spinlock_t          d_lock;           // Protects dentry data
    atomic_t            d_count;          // Reference count
    uint32_t            d_flags;          // Dentry flags
    struct list_head    d_subscribers;    // VFS Event Subscribers
};

// struct file: Represents an open file description
struct file {
    struct list_head    f_list;           // List of all open files
    atomic_t            f_count;          // Reference count
    struct dentry       *f_dentry;        // Dentry of the file
    struct inode        *f_inode;         // Inode of the file
    const struct file_operations *f_op;   // File operations (can be inherited from inode)
    vfs_loff_t          f_pos;            // Current file offset
    uint32_t            f_flags;          // Open flags (O_RDONLY, O_WRONLY, etc.)
    uint32_t            f_mode;           // Internal mode (FMODE_READ, etc.)
    void                *private_data;    // Filesystem private data for this open file
    struct resdomain    *f_rd;            // ResDomain this file is charged to
};

struct vm_area_struct;
struct wait_queue_head;
struct poll_table_struct;

typedef struct poll_table_struct {
    void (*_qproc)(struct file *, struct wait_queue_head *, struct poll_table_struct *);
} poll_table;

struct dir_context;
typedef int (*filldir_t)(struct dir_context *, const char *, int, vfs_loff_t, vfs_ino_t, unsigned int);

struct dir_context {
    filldir_t actor;
    vfs_loff_t pos;
};

// struct file_operations: Operations for an open file
struct file_operations {
    fn(vfs_off_t, llseek, struct file *file, vfs_off_t offset, int whence);
    fn(ssize_t, read, struct file *file, char *buf, size_t count, vfs_loff_t *ppos);
    fn(ssize_t, write, struct file *file, const char *buf, size_t count, vfs_loff_t *ppos);
    fn(int, iterate, struct file *file, struct dir_context *ctx);
    fn(int, mmap, struct file *file, struct vm_area_struct *vma);
    fn(int, open, struct inode *inode, struct file *file);
    fn(int, release, struct inode *inode, struct file *file);
    fn(int, ioctl, struct file *file, unsigned int cmd, unsigned long arg);
    fn(uint32_t, poll, struct file *file, poll_table *pt);
};

// struct inode_operations: Operations for an inode
struct inode_operations {
    fn(int, create, struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
    fn(struct dentry *, lookup, struct inode *dir, struct dentry *dentry, uint32_t flags);
    fn(int, link, struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
    fn(int, unlink, struct inode *dir, struct dentry *dentry);
    fn(int, mkdir, struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
    fn(int, rmdir, struct inode *dir, struct dentry *dentry);
    fn(int, rename, struct inode *old_dir, struct dentry *old_dentry,
                         struct inode *new_dir, struct dentry *new_dentry);
    fn(int, setattr, struct dentry *dentry, vfs_mode_t mode, vfs_loff_t size);
    fn(int, getattr, struct dentry *dentry, struct inode *inode);
    fn(int, symlink, struct inode *dir, struct dentry *dentry, const char *oldname);
    fn(ssize_t, readlink, struct dentry *dentry, char *buf, size_t bufsiz);
    fn(const char *, follow_link, struct dentry *dentry, void **cookie);
    fn(void, put_link, struct dentry *dentry, void *cookie);
};

// struct super_operations: Operations for a superblock
struct super_operations {
    fn(struct inode*, alloc_inode, struct super_block *sb);
    fn(void, destroy_inode, struct inode *inode);
    fn(void, dirty_inode, struct inode *inode); // Mark inode as dirty
    fn(int, write_inode, struct inode *inode, int sync);
    fn(void, put_super, struct super_block *sb);
    fn(int, statfs, struct dentry *dentry, void *buf); // Fill fs statistics
    // Add more operations as needed, e.g., remount, show_options
};

// Structure to register a filesystem type
struct file_system_type {
  const char *name;
  fn(int, mount, struct file_system_type *fs_type, const char *dev_name, const char *dir_name, unsigned long flags, void *data);
  fn(void, kill_sb, struct super_block *sb);
  struct list_head fs_list; // List of registered filesystems
};

struct mount {
    struct list_head mnt_list;
    struct dentry *mnt_mountpoint;
    struct dentry *mnt_root;
    struct super_block *mnt_sb;
    struct mount *mnt_parent;
};

struct stat {
    dev_t         st_dev;      /* ID of device containing file */
    vfs_ino_t     st_ino;      /* Inode number */
    vfs_mode_t    st_mode;     /* File type and mode */
    vfs_nlink_t   st_nlink;    /* Number of hard links */
    uid_t         st_uid;      /* User ID of owner */
    gid_t         st_gid;      /* Group ID of owner */
    dev_t         st_rdev;     /* Device ID (if special file) */
    vfs_loff_t    st_size;     /* Total size, in bytes */
    struct timespec st_atim;   /* Time of last access */
    struct timespec st_mtim;   /* Time of last modification */
    struct timespec st_ctim;   /* Time of last status change */
    uint64_t      st_blksize;  /* Block size for filesystem I/O */
    uint64_t      st_blocks;   /* Number of 512B blocks allocated */
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020

int vfs_mount(const char *dev_name, const char *dir_name, const char *type, unsigned long flags, void *data);
int vfs_init(void);
int register_filesystem(struct file_system_type *fs_type);
int unregister_filesystem(struct file_system_type *fs_type);

int vfs_mmap(struct file *file, struct vm_area_struct *vma);
int generic_file_mmap(struct file *file, struct vm_area_struct *vma);

int vfs_stat(const char *path, struct stat *statbuf);
int vfs_fstat(struct file *file, struct stat *statbuf);
int vfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
uint32_t vfs_poll(struct file *file, poll_table *pt);

struct inode *new_inode(struct super_block *sb);
void iput(struct inode *inode);
void iget(struct inode *inode);

void dput(struct dentry *dentry);
struct dentry *dget(struct dentry *dentry);
struct dentry *d_alloc_pseudo(struct super_block *sb, const struct qstr *name);

ssize_t simple_read_from_buffer(void *to, size_t count, vfs_loff_t *ppos, const void *from, size_t available);
struct dentry *simple_lookup(struct inode *dir, struct dentry *dentry, uint32_t flags);
int simple_rmdir(struct inode *dir, struct dentry *dentry);

int vfs_mkdir(struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
int vfs_mknod(struct inode *dir, struct dentry *dentry, vfs_mode_t mode, dev_t dev);
int vfs_create(struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
int vfs_unlink(struct inode *dir, struct dentry *dentry);
int vfs_rmdir(struct inode *dir, struct dentry *dentry);
int vfs_rename(struct inode *old_dir, struct dentry *old_dentry,
               struct inode *new_dir, struct dentry *new_dentry);
int vfs_symlink(struct inode *dir, struct dentry *dentry, const char *oldname);
int vfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz);

void init_special_inode(struct inode *inode, vfs_mode_t mode, dev_t rdev);

int do_mkdir(const char *path, vfs_mode_t mode);
int do_mknod(const char *path, vfs_mode_t mode, dev_t dev);
int do_unlink(const char *path);
int do_rmdir(const char *path);
int do_rename(const char *oldpath, const char *newpath);
int do_symlink(const char *oldpath, const char *newpath);
ssize_t do_readlink(const char *path, char *buf, size_t bufsiz);

struct linux_dirent64 {
    uint64_t        d_ino;    /* 64-bit inode number */
    int64_t         d_off;    /* 64-bit offset to next structure */
    unsigned short  d_reclen; /* Size of this dirent */
    unsigned char   d_type;   /* File type */
    char            d_name[]; /* Filename (null-terminated) */
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

int sys_chdir(const char *path);
char *sys_getcwd(char *buf, size_t size);
int sys_getdents64(unsigned int fd, struct linux_dirent64 *dirent, unsigned int count);
int sys_mkdir(const char *path, vfs_mode_t mode);
int sys_mknod(const char *path, vfs_mode_t mode, dev_t dev);
int sys_unlink(const char *path);
int sys_rmdir(const char *path);
int sys_rename(const char *oldpath, const char *newpath);
int sys_symlink(const char *oldpath, const char *newpath);
ssize_t sys_readlink(const char *path, char *buf, size_t bufsiz);
int sys_chmod(const char *path, vfs_mode_t mode);
int sys_chown(const char *path, uid_t owner, gid_t group);
int sys_truncate(const char *path, vfs_loff_t length);
int sys_ftruncate(int fd, vfs_loff_t length);
int sys_mount(const char *dev_name, const char *dir_name, const char *type, unsigned long flags, void *data);

#define VFS_EVENT_CREATE 0x01
#define VFS_EVENT_DELETE 0x02
#define VFS_EVENT_MODIFY 0x04
#define VFS_EVENT_ATTRIB 0x08

void vfs_notify_change(struct dentry *dentry, uint32_t event);

struct timespec current_time(struct inode *inode);
