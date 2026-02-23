# AeroSync Virtual File System (VFS)

AeroSync implements a Linux-inspired Virtual File System (VFS) that provides a common interface for different filesystem types and devices.

## Core Abstractions

### 1. `struct inode`
Represents a physical object on the filesystem (file, directory, device node). It contains metadata and pointers to operations (`inode_operations`, `file_operations`).

### 2. `struct dentry`
Represents a directory entry, linking a name to an inode. AeroSync uses a dentry cache to speed up path lookups.

### 3. `struct file`
Represents an open file description in a process. It tracks the current offset and access flags.

### 4. `struct super_block`
Represents a mounted filesystem instance.

## Everything is a File

AeroSync follows the "everything is a file" philosophy by integrating the **Unified Driver Model (UDM)** with **devtmpfs**.

- **Device Nodes**: When a UDM `struct device` is registered with a class that has `CLASS_FLAG_AUTO_DEVTMPFS`, a corresponding node is automatically created in `/dev`.
- **Character Devices**: Drivers provide `struct char_operations`, which are mapped to VFS `file_operations` via `init_special_inode`.

## Process Integration

Each `task_struct` contains:
- `files`: A `struct files_struct` managing the file descriptor table.
- `fs`: A `struct fs_struct` managing the root and current working directory (`cwd`).

## Planned Features
- **Relative Path Lookup**: Support for `.` and `..` in `vfs_path_lookup`.
- **Mount Namespaces**: Per-process or global mount management.
- **Unified Buffer Cache (UBC)**: Full integration between the page cache and block layer.
