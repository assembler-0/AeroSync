#ifndef VFS_H
#define VFS_H

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <linux/list.h>

// Forward declarations for VFS structures
struct super_block;
struct inode;
struct dentry;
struct file;

// VFS specific types
typedef uint64_t vfs_ino_t;      // Inode number type
typedef uint64_t vfs_off_t;      // Offset type (for file positions)
typedef uint32_t vfs_mode_t;     // File mode/permissions type
typedef uint32_t vfs_nlink_t;    // Number of links type
typedef uint64_t vfs_loff_t;     // Large file offset type
typedef uint32_t uid_t;          // User ID
typedef uint32_t gid_t;          // Group ID

// time_t and timespec might be defined elsewhere, but for now define here
typedef long time_t;

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
    uint32_t            s_blocksize;      // Block size in bytes
    // ... more fields like device, flags, etc.
};

// struct inode: Represents a file or directory
struct inode {
    struct list_head    i_list;           // All inodes in use list (global)
    struct list_head    i_dentry;         // List of dentries referencing this inode
    spinlock_t          i_lock;           // Protects inode data
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
    const struct inode_operations *i_op;  // Inode operations
    const struct file_operations  *i_fop; // Default file operations
    void                *i_fs_info;       // Filesystem private data
    // ... more fields for devices, etc.
};

// struct dentry: Represents a directory entry (filename in a directory)
struct dentry {
    struct list_head    d_hash;           // Hash list for dentry cache
    struct list_head    d_lru;            // LRU list for dentry cache (if implementing LRU)
    struct list_head    d_child;          // List of children in parent dentry
    struct list_head    d_subdirs;        // List of subdirectories (if this is a directory dentry)
    struct dentry       *d_parent;        // Parent dentry
    struct qstr         d_name;           // Name of this dentry
    struct inode        *d_inode;         // Inode corresponding to this dentry (or NULL if negative dentry)
    spinlock_t          d_lock;           // Protects dentry data
    uint32_t            d_flags;          // Dentry flags
    // ... more fields for mounted state, etc.
};

// struct file: Represents an open file description
struct file {
    struct list_head    f_list;           // List of all open files
    struct dentry       *f_dentry;        // Dentry of the file
    struct inode        *f_inode;         // Inode of the file
    const struct file_operations *f_op;   // File operations (can be inherited from inode)
    vfs_loff_t          f_pos;            // Current file offset
    uint32_t            f_flags;          // Open flags (O_RDONLY, O_WRONLY, etc.)
    void                *private_data;    // Filesystem private data for this open file
    // ... more fields like reference count, etc.
};

// struct file_operations: Operations for an open file
struct file_operations {
    vfs_off_t (*llseek) (struct file *file, vfs_off_t offset, int whence);
    ssize_t (*read)     (struct file *file, char *buf, size_t count, vfs_loff_t *ppos);
    ssize_t (*write)    (struct file *file, const char *buf, size_t count, vfs_loff_t *ppos);
    int (*open)         (struct inode *inode, struct file *file);
    int (*release)      (struct inode *inode, struct file *file);
    // Add more operations as needed, e.g., ioctl, mmap, poll, etc.
};

// struct inode_operations: Operations for an inode
struct inode_operations {
    int (*create)       (struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
    struct dentry *(*lookup)    (struct inode *dir, struct dentry *dentry, uint32_t flags);
    int (*link)         (struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
    int (*unlink)       (struct inode *dir, struct dentry *dentry);
    int (*mkdir)        (struct inode *dir, struct dentry *dentry, vfs_mode_t mode);
    int (*rmdir)        (struct inode *dir, struct dentry *dentry);
    int (*rename)       (struct inode *old_dir, struct dentry *old_dentry,
                         struct inode *new_dir, struct dentry *new_dentry);
    int (*setattr)      (struct dentry *dentry, vfs_mode_t mode, vfs_loff_t size);
    int (*getattr)      (struct dentry *dentry, struct inode *inode);
    // Add more operations as needed, e.g., symlink, readlink, follow_link, permission
};

// struct super_operations: Operations for a superblock
struct super_operations {
    struct inode *(*alloc_inode)    (struct super_block *sb);
    void (*destroy_inode)           (struct inode *inode);
    void (*dirty_inode)             (struct inode *inode); // Mark inode as dirty
    int (*write_inode)              (struct inode *inode, int sync);
    void (*put_super)               (struct super_block *sb);
    int (*statfs)                   (struct dentry *dentry, void *buf); // Fill fs statistics
    // Add more operations as needed, e.g., remount, show_options
};

// Structure to register a filesystem type
struct file_system_type {
  const char *name;
  int (*mount)(struct file_system_type *fs_type, const char *dev_name, const char *dir_name, unsigned long flags, void *data);
  void (*kill_sb)(struct super_block *sb);
  struct list_head fs_list; // List of registered filesystems
};

void vfs_init(void);
int register_filesystem(struct file_system_type *fs_type);
int unregister_filesystem(struct file_system_type *fs_type);

#endif // VFS_H
