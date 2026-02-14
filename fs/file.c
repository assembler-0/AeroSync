/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/file.c
 * @brief File management
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <lib/bitmap.h>
#include <aerosync/panic.h>
#include <aerosync/export.h>
#include <aerosync/errno.h>

struct files_struct init_files = {
  .count = ATOMIC_INIT(1),
  .file_lock = SPINLOCK_INIT,
  .next_fd = 0,
  .fdtab = {
    .max_fds = NR_OPEN_DEFAULT,
  }
};

// We need a way to initialize the pointers that depend on the struct address
void files_init(void) {
  init_files.fdtab.fd = init_files.fd_array;
  init_files.fdtab.open_fds = init_files.open_fds_init;
  init_files.fdtab.close_on_exec = init_files.close_on_exec_init;
}

struct file *fget(unsigned int fd) {
  struct files_struct *files = current->files;
  struct file *file = nullptr;

  if (!files) return nullptr;

  spinlock_lock(&files->file_lock);
  if (fd < files->fdtab.max_fds) {
    file = files->fdtab.fd[fd];
    if (file) {
      atomic_inc(&file->f_count);
    }
  }
  spinlock_unlock(&files->file_lock);

  return file;
}

void fput(struct file *file) {
  if (!file) return;

  if (atomic_dec_and_test(&file->f_count)) {
    vfs_close(file);
  }
}

int fd_install(unsigned int fd, struct file *file) {
  struct files_struct *files = current->files;
  if (!files) return -EBADF;

  spinlock_lock(&files->file_lock);
  if (fd >= files->fdtab.max_fds) {
    spinlock_unlock(&files->file_lock);
    return -EMFILE;
  }
  files->fdtab.fd[fd] = file;
  // Initial refcount is already set when file is created (vfs_open)
  spinlock_unlock(&files->file_lock);
  return 0;
}

int sys_dup2(int oldfd, int newfd) {
  struct files_struct *files = current->files;
  if (!files) return -ENOSYS;

  if (oldfd == newfd) return newfd;

  struct file *file = fget(oldfd);
  if (!file) return -EBADF;

  spinlock_lock(&files->file_lock);
  if (newfd < 0 || newfd >= (int) files->fdtab.max_fds) {
    spinlock_unlock(&files->file_lock);
    fput(file);
    return -EBADF;
  }

  struct file *to_close = files->fdtab.fd[newfd];
  files->fdtab.fd[newfd] = file;
  set_bit(newfd, files->fdtab.open_fds);
  clear_bit(newfd, files->fdtab.close_on_exec);
  spinlock_unlock(&files->file_lock);

  if (to_close) fput(to_close);

  return newfd;
}

EXPORT_SYMBOL(sys_dup2);

int sys_dup(int oldfd) {
  struct file *file = fget(oldfd);
  if (!file) return -EBADF;

  int newfd = get_unused_fd_flags(0);
  if (newfd < 0) {
    fput(file);
    return -EMFILE;
  }

  fd_install(newfd, file);
  return newfd;
}

EXPORT_SYMBOL(sys_dup);

int sys_fcntl(int fd, unsigned int cmd, unsigned long arg) {
  struct files_struct *files = current->files;
  if (!files || fd < 0 || fd >= (int) files->fdtab.max_fds) {
    return -EBADF;
  }

  int ret = -EINVAL;
  spinlock_lock(&files->file_lock);

  if (!test_bit(fd, files->fdtab.open_fds)) {
    spinlock_unlock(&files->file_lock);
    return -EBADF;
  }

  switch (cmd) {
    case 0: /* F_DUPFD */
    {
      struct file *file = files->fdtab.fd[fd];
      int start_fd = (int)arg;
      if (start_fd < 0 || start_fd >= (int)files->fdtab.max_fds) {
        ret = -EINVAL;
        break;
      }
      
      int newfd = find_next_zero_bit(files->fdtab.open_fds, files->fdtab.max_fds, start_fd);
      if (newfd >= (int) files->fdtab.max_fds) {
        ret = -EMFILE;
        break;
      }
      
      set_bit(newfd, files->fdtab.open_fds);
      files->fdtab.fd[newfd] = file;
      if (file) atomic_inc(&file->f_count);
      ret = newfd;
      break;
    }
    case 1: /* F_GETFD */
      ret = test_bit(fd, files->fdtab.close_on_exec) ? 1 : 0;
      break;
    case 2: /* F_SETFD */
      if (arg & 1) set_bit(fd, files->fdtab.close_on_exec);
      else clear_bit(fd, files->fdtab.close_on_exec);
      ret = 0;
      break;
    default:
      ret = -EINVAL;
  }

  spinlock_unlock(&files->file_lock);
  return ret;
}

EXPORT_SYMBOL(sys_fcntl);

int get_unused_fd_flags(unsigned int flags) {
  struct files_struct *files = current->files;
  if (!files) return -EBADF;

  spinlock_lock(&files->file_lock);
  int fd = find_next_zero_bit(files->fdtab.open_fds, files->fdtab.max_fds, files->next_fd);

  if (fd >= files->fdtab.max_fds) {
    // Here we would normally expand the fd table
    spinlock_unlock(&files->file_lock);
    return -EMFILE;
  }

  set_bit(fd, files->fdtab.open_fds);
  if (flags & O_CLOEXEC) {
    set_bit(fd, files->fdtab.close_on_exec);
  } else {
    clear_bit(fd, files->fdtab.close_on_exec);
  }

  files->next_fd = fd + 1;
  spinlock_unlock(&files->file_lock);
  return fd;
}

void put_unused_fd(unsigned int fd) {
  struct files_struct *files = current->files;
  if (!files) return;

  spinlock_lock(&files->file_lock);
  if (fd < files->fdtab.max_fds) {
    clear_bit(fd, files->fdtab.open_fds);
    if (fd < files->next_fd) {
      files->next_fd = fd;
    }
  }
  spinlock_unlock(&files->file_lock);
}

ssize_t kernel_read(struct file *file, void *buf, size_t count, vfs_loff_t *pos) {
    uint32_t old_mode = file->f_mode;
    file->f_mode |= 0x1000; // Internal flag for kernel buffer
    ssize_t ret = vfs_read(file, buf, count, pos);
    file->f_mode = old_mode;
    return ret;
}
EXPORT_SYMBOL(kernel_read);

ssize_t kernel_write(struct file *file, const void *buf, size_t count, vfs_loff_t *pos) {
    uint32_t old_mode = file->f_mode;
    file->f_mode |= 0x1000; // Internal flag for kernel buffer
    ssize_t ret = vfs_write(file, buf, count, pos);
    file->f_mode = old_mode;
    return ret;
}
EXPORT_SYMBOL(kernel_write);

// Helper to allocate and initialize a files_struct (for fork)
struct files_struct *copy_files(struct files_struct *old_files) {
  struct files_struct *new_files = kzalloc(sizeof(struct files_struct));
  if (!new_files) return nullptr;

  atomic_set(&new_files->count, 1);
  spinlock_init(&new_files->file_lock);
  new_files->next_fd = 0;
  new_files->fdtab.max_fds = NR_OPEN_DEFAULT;
  new_files->fdtab.fd = new_files->fd_array;
  new_files->fdtab.open_fds = new_files->open_fds_init;
  new_files->fdtab.close_on_exec = new_files->close_on_exec_init;

  if (old_files) {
    spinlock_lock(&old_files->file_lock);
    for (int i = 0; i < old_files->fdtab.max_fds; i++) {
      if (test_bit(i, old_files->fdtab.open_fds)) {
        struct file *f = old_files->fdtab.fd[i];
        new_files->fd_array[i] = f;
        set_bit(i, new_files->open_fds_init);
        if (test_bit(i, old_files->fdtab.close_on_exec))
          set_bit(i, new_files->close_on_exec_init);

        if (f) atomic_inc(&f->f_count);
      }
    }
    new_files->next_fd = old_files->next_fd;
    spinlock_unlock(&old_files->file_lock);
  }

  return new_files;
}