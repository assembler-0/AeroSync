///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/entry/syscall.c
 * @brief System Call Dispatcher and Initialization
 * @copyright (C) 2025 assembler-0
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
#include <mm/slab.h>
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

  if (fd == 1 || fd == 2) {
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
  }

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
  files->fdtab.fd[fd] = NULL;
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

  struct file *file = NULL;
  if (!(flags & MAP_ANON)) {
      file = fget(fd);
      if (!file) {
          REGS_RETURN_VAL(regs, -EBADF);
          return;
      }
      /* TODO: Get vm_object from file/inode */
      fput(file);
      REGS_RETURN_VAL(regs, -ENODEV); // Not yet fully supported for files
      return;
  }

  uint64_t ret = do_mmap(mm, addr, len, prot, flags, file, off >> PAGE_SHIFT);
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

  /* TODO: Implement do_mremap logic in vma.c */
  REGS_RETURN_VAL(regs, -ENOSYS);
}

static sys_call_ptr_t syscall_table[] = {
  [0] = sys_read,
  [1] = sys_write,
  [2] = sys_open,
  [3] = sys_close,
  [8] = sys_lseek,
  [9] = sys_mmap,
  [10] = sys_mprotect,
  [11] = sys_munmap,
  [13] = sys_rt_sigaction,
  [14] = sys_rt_sigprocmask,
  [15] = sys_rt_sigreturn,
  [25] = sys_mremap,
  [39] = sys_getpid_handler,
  [56] = sys_clone_handler,
  [57] = sys_fork_handler,
  [60] = sys_exit_handler,
  [62] = sys_kill,
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
