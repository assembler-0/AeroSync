///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/entry/syscall.c
 * @brief System Call Dispatcher and Initialization
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


#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt/gdt.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/sched/process.h>
#include <aerosync/types.h>
#include <aerosync/sysintf/panic.h>
#include <lib/printk.h>
#include <lib/uaccess.h>
#include <arch/x86_64/entry.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <mm/slub.h>
#include <lib/bitmap.h>
#include <aerosync/signal.h>
#include <mm/vma.h>

#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define MSR_EFER 0xC0000080
#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define EFER_SCE 0x01

extern void syscall_entry(void);

#define REGS_RETURN_VAL(r, v) (r ? r->rax = v : panic(SYSCALL_CLASS "regs == null"))

fnd(void, sys_call_ptr_t, struct syscall_regs *);

static void sys_ni_syscall(struct syscall_regs *regs) {
  printk(KERN_WARNING SYSCALL_CLASS "Unknown syscall %llu\n", regs->rax);
  REGS_RETURN_VAL(regs, -1);
}

static void sys_read(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  char *buf = (char *) regs->rsi;
  size_t count = (size_t) regs->rdx;

  struct file *file = fget(fd);
  if (!file) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  if ((file->f_flags & O_ACCMODE) == O_WRONLY) {
    fput(file);
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  size_t total_read = 0;
  char *kbuf = kmalloc(4096);
  if (!kbuf) {
    fput(file);
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  while (count > 0) {
    size_t to_read = (count > 4096) ? 4096 : count;
    vfs_loff_t pos = file->f_pos;
    ssize_t ret = vfs_read(file, kbuf, to_read, &pos);

    if (ret < 0) {
      if (total_read == 0) total_read = ret;
      break;
    }
    if (ret == 0) break;

    if (copy_to_user(buf + total_read, kbuf, ret) != 0) {
      if (total_read == 0) total_read = -EFAULT;
      break;
    }

    file->f_pos = pos;
    total_read += ret;
    count -= ret;
    if (ret < (ssize_t) to_read) break;
  }

  kfree(kbuf);
  fput(file);
  REGS_RETURN_VAL(regs, total_read);
}

static void sys_write(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  const char *buf = (const char *) regs->rsi;
  size_t count = (size_t) regs->rdx;

#ifdef IMPLICIT_FD12_STDOUT_STDERR
  if (fd == 1 || fd == 2) {
#ifdef IMPLICIT_FD12_STDOUT_STDERR_PRINTK
    // stdout or stderr
    char kbuf[256];
    size_t total = 0;

    while (count > 0) {
      size_t to_copy = (count > sizeof(kbuf) - 1) ? sizeof(kbuf) - 1 : count;
      if (copy_from_user(kbuf, buf, to_copy) != 0) {
        REGS_RETURN_VAL(regs, -EFAULT);
        return;
      }

      kbuf[to_copy] = '\0';
      if (fd == 1) printk(KERN_INFO "%s", kbuf);
      if (fd == 2) printk(KERN_ERR "%s", kbuf);

      buf += to_copy;
      count -= to_copy;
      total += to_copy;
    }
    REGS_RETURN_VAL(regs, total);
    return;
#endif
  }
#endif

  struct file *file = fget(fd);
  if (!file) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  if ((file->f_flags & O_ACCMODE) == O_RDONLY) {
    fput(file);
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  size_t total_written = 0;
  char *kbuf = kmalloc(4096);
  if (!kbuf) {
    fput(file);
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  while (count > 0) {
    size_t to_write = (count > 4096) ? 4096 : count;
    if (copy_from_user(kbuf, buf + total_written, to_write) != 0) {
      if (total_written == 0) total_written = -EFAULT;
      break;
    }

    vfs_loff_t pos = file->f_pos;
    ssize_t ret = vfs_write(file, kbuf, to_write, &pos);
    if (ret < 0) {
      if (total_written == 0) total_written = ret;
      break;
    }

    file->f_pos = pos;
    total_written += ret;
    count -= ret;
    if (ret < (ssize_t) to_write) break;
  }

  kfree(kbuf);
  fput(file);
  REGS_RETURN_VAL(regs, total_written);
}

static void sys_open(struct syscall_regs *regs) {
  const char *filename_user = (const char *) regs->rdi;
  int flags = (int) regs->rsi;
  int mode = (int) regs->rdx;

  char *filename = kmalloc(4096);
  if (!filename) {
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  size_t i = 0;
  for (; i < 4095; i++) {
    if (copy_from_user(&filename[i], &filename_user[i], 1)) {
      kfree(filename);
      REGS_RETURN_VAL(regs, -EFAULT);
      return;
    }
    if (filename[i] == '\0') break;
  }
  filename[i] = '\0';

  struct file *file = vfs_open(filename, flags, mode);
  kfree(filename);

  if (!file) {
    REGS_RETURN_VAL(regs, -ENOENT);
    return;
  }

  int fd = get_unused_fd_flags(flags);
  if (fd < 0) {
    fput(file);
    REGS_RETURN_VAL(regs, -EMFILE);
    return;
  }

  fd_install(fd, file);
  REGS_RETURN_VAL(regs, fd);
}

static void sys_close(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  struct files_struct *files = current->files;

  if (!files || fd < 0 || fd >= (int) files->fdtab.max_fds) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  spinlock_lock(&files->file_lock);
  if (!test_bit(fd, files->fdtab.open_fds)) {
    spinlock_unlock(&files->file_lock);
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  struct file *file = files->fdtab.fd[fd];
  files->fdtab.fd[fd] = nullptr;
  clear_bit(fd, files->fdtab.open_fds);
  if (fd < files->next_fd) {
    files->next_fd = fd;
  }
  spinlock_unlock(&files->file_lock);

  if (file) {
    fput(file);
  }
  REGS_RETURN_VAL(regs, 0);
}

static void sys_lseek(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  vfs_loff_t offset = (vfs_loff_t) regs->rsi;
  int whence = (int) regs->rdx;

  struct file *file = fget(fd);
  if (!file) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  vfs_loff_t ret = vfs_llseek(file, offset, whence);
  fput(file);
  REGS_RETURN_VAL(regs, ret);
}

static void sys_dup_handler(struct syscall_regs *regs) {
  int oldfd = (int) regs->rdi;
  extern int sys_dup(int oldfd);
  REGS_RETURN_VAL(regs, sys_dup(oldfd));
}

static void sys_dup2_handler(struct syscall_regs *regs) {
  int oldfd = (int) regs->rdi;
  int newfd = (int) regs->rsi;
  extern int sys_dup2(int oldfd, int newfd);
  REGS_RETURN_VAL(regs, sys_dup2(oldfd, newfd));
}

static void sys_fcntl_handler(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  unsigned int cmd = (unsigned int) regs->rsi;
  unsigned long arg = (unsigned long) regs->rdx;

  extern int sys_fcntl(int fd, unsigned int cmd, unsigned long arg);
  int ret = sys_fcntl(fd, cmd, arg);
  REGS_RETURN_VAL(regs, ret);
}

static void sys_execve(struct syscall_regs *regs) {
  const char *filename_user = (const char *) regs->rdi;
  char **argv_user = (char **) regs->rsi;
  char **envp_user = (char **) regs->rdx;

  char *filename = kmalloc(4096);
  if (!filename) {
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  if (copy_from_user(filename, filename_user, 4096) != 0) {
    kfree(filename);
    REGS_RETURN_VAL(regs, -EFAULT);
    return;
  }

  int ret = do_execve(filename, argv_user, envp_user);
  kfree(filename);

  REGS_RETURN_VAL(regs, ret);
}

static void sys_ioctl(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  unsigned int cmd = (unsigned int) regs->rsi;
  unsigned long arg = (unsigned long) regs->rdx;

  struct file *file = fget(fd);
  if (!file) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  int ret = vfs_ioctl(file, cmd, arg);
  fput(file);

  REGS_RETURN_VAL(regs, ret);
}

static void sys_exit_handler(struct syscall_regs *regs) {
  int status = (int) regs->rdi;
  printk(KERN_DEBUG SYSCALL_CLASS "User process %d exited with status %d\n", current->pid, status);
  sys_exit(status);
}

static void sys_fork_handler(struct syscall_regs *regs) {
  REGS_RETURN_VAL(regs, do_fork(0, 0, regs));
}

static void sys_clone_handler(struct syscall_regs *regs) {
  uint64_t flags = regs->rdi;
  uint64_t stack = regs->rsi;
  REGS_RETURN_VAL(regs, do_fork(flags, stack, regs));
}

static void sys_getpid_handler(struct syscall_regs *regs) {
  REGS_RETURN_VAL(regs, current->pid);
}

static void sys_mmap(struct syscall_regs *regs) {
  uint64_t addr = regs->rdi;
  size_t len = regs->rsi;
  uint64_t prot = regs->rdx;
  uint64_t flags = regs->r10;
  int fd = (int) regs->r8;
  uint64_t off = regs->r9;

  struct mm_struct *mm = current->mm;
  if (!mm) {
    REGS_RETURN_VAL(regs, -EINVAL);
    return;
  }

  struct file *file = nullptr;
  if (!(flags & MAP_ANON)) {
    file = fget(fd);
    if (!file) {
      REGS_RETURN_VAL(regs, -EBADF);
      return;
    }
  }

  uint64_t ret = do_mmap(mm, addr, len, prot, flags, file, nullptr, off >> PAGE_SHIFT);

  if (file) {
    fput(file);
  }

  REGS_RETURN_VAL(regs, ret);
}

static void sys_munmap(struct syscall_regs *regs) {
  uint64_t addr = regs->rdi;
  size_t len = regs->rsi;

  struct mm_struct *mm = current->mm;
  if (!mm) {
    REGS_RETURN_VAL(regs, -EINVAL);
    return;
  }

  int ret = do_munmap(mm, addr, len);
  REGS_RETURN_VAL(regs, ret);
}

static void sys_mprotect(struct syscall_regs *regs) {
  uint64_t addr = regs->rdi;
  size_t len = regs->rsi;
  uint64_t prot = regs->rdx;

  struct mm_struct *mm = current->mm;
  if (!mm) {
    REGS_RETURN_VAL(regs, -EINVAL);
    return;
  }

  int ret = do_mprotect(mm, addr, len, prot);
  REGS_RETURN_VAL(regs, ret);
}

static void sys_mremap(struct syscall_regs *regs) {
  uint64_t old_addr = regs->rdi;
  size_t old_len = regs->rsi;
  size_t new_len = regs->rdx;
  int flags = (int) regs->r10;
  uint64_t new_addr_hint = regs->r8;

  struct mm_struct *mm = current->mm;
  if (!mm) {
    REGS_RETURN_VAL(regs, -EINVAL);
    return;
  }

  uint64_t ret = do_mremap(mm, old_addr, old_len, new_len, flags, new_addr_hint);
  REGS_RETURN_VAL(regs, ret);
}

static void sys_stat(struct syscall_regs *regs) {
  const char *path_user = (const char *) regs->rdi;
  struct stat *statbuf_user = (struct stat *) regs->rsi;

  char *path = kmalloc(4096);
  if (!path) {
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  if (copy_from_user(path, path_user, 4096) != 0) {
    kfree(path);
    REGS_RETURN_VAL(regs, -EFAULT);
    return;
  }

  struct stat st;
  int ret = vfs_stat(path, &st);
  kfree(path);

  if (ret == 0) {
    if (copy_to_user(statbuf_user, &st, sizeof(struct stat)) != 0) {
      REGS_RETURN_VAL(regs, -EFAULT);
      return;
    }
  }

  REGS_RETURN_VAL(regs, ret);
}

static void sys_fstat(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  struct stat *statbuf_user = (struct stat *) regs->rsi;

  struct file *file = fget(fd);
  if (!file) {
    REGS_RETURN_VAL(regs, -EBADF);
    return;
  }

  struct stat st;
  int ret = vfs_fstat(file, &st);
  fput(file);

  if (ret == 0) {
    if (copy_to_user(statbuf_user, &st, sizeof(struct stat)) != 0) {
      REGS_RETURN_VAL(regs, -EFAULT);
      return;
    }
  }

  REGS_RETURN_VAL(regs, ret);
}

static void sys_poll(struct syscall_regs *regs) {
  struct pollfd *fds_user = (struct pollfd *) regs->rdi;
  unsigned int nfds = (unsigned int) regs->rsi;
  int timeout_ms = (int) regs->rdx;

  if (nfds > 1024) {
    REGS_RETURN_VAL(regs, -EINVAL);
    return;
  }

  struct pollfd *fds = kmalloc(nfds * sizeof(struct pollfd));
  if (!fds) {
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  if (copy_from_user(fds, fds_user, nfds * sizeof(struct pollfd)) != 0) {
    kfree(fds);
    REGS_RETURN_VAL(regs, -EFAULT);
    return;
  }

  uint64_t timeout_ns = (uint64_t)-1;
  if (timeout_ms >= 0) {
      timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
  }

  extern int do_poll(struct pollfd *fds, unsigned int nfds, uint64_t timeout_ns);
  int count = do_poll(fds, nfds, timeout_ns);

  if (count >= 0) {
    if (copy_to_user(fds_user, fds, nfds * sizeof(struct pollfd)) != 0) {
      kfree(fds);
      REGS_RETURN_VAL(regs, -EFAULT);
      return;
    }
  }

  kfree(fds);
  REGS_RETURN_VAL(regs, count);
}

static void sys_pipe(struct syscall_regs *regs) {
  int *pipefd_user = (int *) regs->rdi;
  int pipefd[2];

  extern int do_pipe(int pipefd[2]);
  int ret = do_pipe(pipefd);

  if (ret == 0) {
    if (copy_to_user(pipefd_user, pipefd, sizeof(pipefd)) != 0) {
      /* In a real kernel, we would close the FDs on failure */
      REGS_RETURN_VAL(regs, -EFAULT);
      return;
    }
  }

  REGS_RETURN_VAL(regs, ret);
}

static void sys_mkdir_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  vfs_mode_t mode = (vfs_mode_t) regs->rsi;
  REGS_RETURN_VAL(regs, sys_mkdir(path, mode));
}

static void sys_mknod_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  vfs_mode_t mode = (vfs_mode_t) regs->rsi;
  dev_t dev = (dev_t) regs->rdx;
  REGS_RETURN_VAL(regs, sys_mknod(path, mode, dev));
}

static void sys_chdir_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  REGS_RETURN_VAL(regs, sys_chdir(path));
}

static void sys_getcwd_handler(struct syscall_regs *regs) {
  char *buf = (char *) regs->rdi;
  size_t size = (size_t) regs->rsi;
  char *ret = sys_getcwd(buf, size);
  REGS_RETURN_VAL(regs, (uint64_t)ret);
}

static void sys_unlink_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  REGS_RETURN_VAL(regs, sys_unlink(path));
}

static void sys_rmdir_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  REGS_RETURN_VAL(regs, sys_rmdir(path));
}

static void sys_rename_handler(struct syscall_regs *regs) {
  const char *oldpath = (const char *) regs->rdi;
  const char *newpath = (const char *) regs->rsi;
  REGS_RETURN_VAL(regs, sys_rename(oldpath, newpath));
}

static void sys_symlink_handler(struct syscall_regs *regs) {
  const char *oldpath = (const char *) regs->rdi;
  const char *newpath = (const char *) regs->rsi;
  REGS_RETURN_VAL(regs, sys_symlink(oldpath, newpath));
}

static void sys_readlink_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  char *buf = (char *) regs->rsi;
  size_t bufsiz = (size_t) regs->rdx;
  REGS_RETURN_VAL(regs, sys_readlink(path, buf, bufsiz));
}

static void sys_chmod_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  vfs_mode_t mode = (vfs_mode_t) regs->rsi;
  REGS_RETURN_VAL(regs, sys_chmod(path, mode));
}

static void sys_chown_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  uid_t owner = (uid_t) regs->rsi;
  gid_t group = (gid_t) regs->rdx;
  REGS_RETURN_VAL(regs, sys_chown(path, owner, group));
}

static void sys_truncate_handler(struct syscall_regs *regs) {
  const char *path = (const char *) regs->rdi;
  vfs_loff_t length = (vfs_loff_t) regs->rsi;
  REGS_RETURN_VAL(regs, sys_truncate(path, length));
}

static void sys_ftruncate_handler(struct syscall_regs *regs) {
  int fd = (int) regs->rdi;
  vfs_loff_t length = (vfs_loff_t) regs->rsi;
  REGS_RETURN_VAL(regs, sys_ftruncate(fd, length));
}

static void sys_mount_handler(struct syscall_regs *regs) {
  const char *dev_name = (const char *) regs->rdi;
  const char *dir_name = (const char *) regs->rsi;
  const char *type = (const char *) regs->rdx;
  unsigned long flags = (unsigned long) regs->r10;
  void *data = (void *) regs->r8;
  REGS_RETURN_VAL(regs, sys_mount(dev_name, dir_name, type, flags, data));
}

static sys_call_ptr_t syscall_table[] = {
  [0] = sys_read,
  [1] = sys_write,
  [2] = sys_open,
  [3] = sys_close,
  [4] = sys_stat,
  [5] = sys_fstat,
  [7] = sys_poll,
  [8] = sys_lseek,
  [9] = sys_mmap,
  [10] = sys_mprotect,
  [11] = sys_munmap,
  [13] = sys_rt_sigaction,
  [14] = sys_rt_sigprocmask,
  [15] = sys_rt_sigreturn,
  [16] = sys_ioctl,
  [32] = sys_dup_handler,
  [33] = sys_dup2_handler,
  [72] = sys_fcntl_handler,
  [22] = sys_pipe,
  [25] = sys_mremap,
  [39] = sys_getpid_handler,
  [56] = sys_clone_handler,
  [57] = sys_fork_handler,
  [59] = sys_execve,
  [60] = sys_exit_handler,
  [62] = sys_kill,
  [76] = sys_truncate_handler,
  [77] = sys_ftruncate_handler,
  [79] = sys_getcwd_handler,
  [80] = sys_chdir_handler,
  [82] = sys_rename_handler,
  [83] = sys_mkdir_handler,
  [84] = sys_rmdir_handler,
  [87] = sys_unlink_handler,
  [88] = sys_symlink_handler,
  [89] = sys_readlink_handler,
  [90] = sys_chmod_handler,
  [92] = sys_chown_handler,
  [133] = sys_mknod_handler,
  [165] = sys_mount_handler,
  [200] = sys_tkill,
  [234] = sys_tgkill,
};

#define NR_SYSCALLS (sizeof(syscall_table) / sizeof(sys_call_ptr_t))

void do_syscall(struct syscall_regs *regs) {
  uint64_t syscall_num = regs->rax;

  if (syscall_num >= NR_SYSCALLS || !syscall_table[syscall_num]) {
    sys_ni_syscall(regs);
  } else {
    syscall_table[syscall_num](regs);
  }

  /* Check for pending signals before returning to user space */
  do_signal(regs, true);
}

void syscall_init(void) {
  // 1. Enable SCE (Syscall Extensions) in EFER
  uint64_t efer = rdmsr(MSR_EFER);
  efer |= EFER_SCE;
  wrmsr(MSR_EFER, efer);

  // 2. Setup STAR (Segment Target Address Register)
  // Bits 63-48: Sysret CS (User Code - 16).
  // Bits 47-32: Syscall CS (Kernel Code).
  uint64_t star = 0;
  star |= ((uint64_t) KERNEL_DATA_SELECTOR << 48); // User Base (0x10) -> CS=0x20, SS=0x18
  star |= ((uint64_t) KERNEL_CODE_SELECTOR << 32); // Kernel Base (0x08) -> CS=0x08, SS=0x10
  wrmsr(MSR_STAR, star);

  // 3. Setup LSTAR (Long Mode Syscall Target Address)
  wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);

  // 4. Setup SFMASK (RFLAGS Mask)
  // Mask interrupts (IF=0x200), Direction (DF=0x400)
  wrmsr(MSR_FMASK, 0x200);

  // 5. Initialize KERNEL_GS_BASE with current GS_BASE
  // This ensures that the first swapgs in enter_ring3 has a valid kernel GS to swap back.
  wrmsr(MSR_KERNEL_GS_BASE, rdmsr(MSR_GS_BASE));
}
