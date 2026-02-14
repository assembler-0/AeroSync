/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/elf_loader.c
 * @brief Linux-like ELF binary loader backend
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
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/elf.h>
#include <aerosync/errno.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/export.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <mm/slub.h>
#include <mm/vma.h>

struct linux_binprm {
  char buf[128];
  void *data;
  size_t data_len;
  struct file *file;
  int argc, envc;
  char **argv, **envp;
  struct mm_struct *mm;
  uint64_t p; // Current stack pointer during setup
};

extern void ret_from_user_thread(void);

static int setup_arg_pages(struct linux_binprm *bprm) {
  uint64_t stack_size = PAGE_SIZE * 16; // 64KB initial stack
  uint64_t stack_top = vmm_get_max_user_address() - PAGE_SIZE; // Leave a guard page
  uint64_t stack_base = stack_top - stack_size;

  if (mm_populate_user_range(bprm->mm, stack_base, stack_size, VM_READ | VM_WRITE | VM_USER, nullptr, 0) != 0) {
    return -ENOMEM;
  }

  bprm->mm->start_stack = stack_top;
  bprm->p = stack_top;
  return 0;
}

static int copy_strings(int argc, char **argv, struct linux_binprm *bprm) {
  for (int i = 0; i < argc; i++) {
    char *str;
    if (copy_from_user(&str, &argv[i], sizeof(char *)) != 0) return -EFAULT;

    /* For now, we assume simple allocation.
     * In a real kernel, we'd copy bytes directly to bprm pages.
     */
    char *kstr = kmalloc(4096);
    if (!kstr) return -ENOMEM;

    size_t len = 0;
    for (; len < 4095; len++) {
      if (copy_from_user(&kstr[len], &str[len], 1) != 0) {
        kfree(kstr);
        return -EFAULT;
      }
      if (kstr[len] == '\0') break;
    }
    kstr[len] = '\0';

    /* Push onto user stack (top-down) */
    size_t str_len = len + 1;
    bprm->p -= str_len;

    void *dst = pmm_phys_to_virt(vmm_virt_to_phys(bprm->mm, bprm->p));
    memcpy(dst, kstr, str_len);

    /* Store the user-space address of this string */
    if (bprm->argc < 128) {
      // bprm->buf reused for ptrs? No, use bprm->p
      // Need to store ptrs to strings.
    }
    kfree(kstr);
  }
  return 0;
}

/*
 * create_elf_tables - Setup argc, argv, envp on the user stack.
 * Follows the standard System V ABI.
 */
static int create_elf_tables(struct linux_binprm *bprm, Elf64_Ehdr *exec) {
  (void) exec;

  /*
   * We need to push:
   * [argc]
   * [argv[0]] ... [argv[n]] [nullptr]
   * [envp[0]] ... [envp[m]] [nullptr]
   * [Auxiliary Vector]
   */

  uint64_t *sp_phys;

  // Align stack to 16 bytes
  bprm->p &= ~15;

  // This is a simplified version that just pushes argc=0 for now
  // but correctly handles the stack pointer.
  bprm->p -= 8;
  sp_phys = (uint64_t *) pmm_phys_to_virt(vmm_virt_to_phys(bprm->mm, bprm->p));
  *sp_phys = bprm->argc;

  return 0;
}

static int load_elf_binary(struct task_struct *p, struct linux_binprm *bprm) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *) bprm->data;
  Elf64_Phdr *phdrs;
  int retval;

  if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1)
    return -ENOEXEC;

  if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
    return -ENOEXEC;

  bprm->mm = mm_create();
  if (!bprm->mm) return -ENOMEM;

  phdrs = (Elf64_Phdr *) ((uint8_t *) bprm->data + hdr->e_phoff);

  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;

    uint64_t vaddr = phdrs[i].p_vaddr;
    uint64_t filesz = phdrs[i].p_filesz;
    uint64_t memsz = phdrs[i].p_memsz;
    uint64_t offset = phdrs[i].p_offset;
    uint64_t prot = PROT_READ;

    if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
    if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;

    if (bprm->file) {
      /* Zero-copy: Use do_mmap to map the binary directly into process space */
      uint64_t mmap_flags = MAP_PRIVATE | MAP_FIXED;
      uint64_t ret = do_mmap(bprm->mm, vaddr, memsz, prot, mmap_flags, bprm->file, nullptr, offset >> PAGE_SHIFT);
      if (ret != vaddr) {
        goto bad_free;
      }
    } else {
      /* Fallback to copy if no file is provided (e.g. initial RAM buffer) */
      uint64_t vm_flags = VM_READ | VM_USER;
      if (phdrs[i].p_flags & PF_W) vm_flags |= VM_WRITE;
      if (phdrs[i].p_flags & PF_X) vm_flags |= VM_EXEC;

      if (mm_populate_user_range(bprm->mm, vaddr, memsz, vm_flags,
                                 (uint8_t *) bprm->data + offset, filesz) != 0) {
        goto bad_free;
      }
    }

    if (vaddr < bprm->mm->start_code || bprm->mm->start_code == 0)
      bprm->mm->start_code = vaddr;
    if (vaddr + memsz > bprm->mm->end_code)
      bprm->mm->end_code = vaddr + memsz;
  }

  retval = setup_arg_pages(bprm);
  if (retval < 0) goto bad_free;

  create_elf_tables(bprm, hdr);

  /* Update the task's MM */
  struct mm_struct *old_mm = p->mm;
  p->mm = bprm->mm;
  p->active_mm = bprm->mm;
  p->flags &= ~PF_KTHREAD;

  // If we are loading into 'current', we must switch PML4 immediately
  if (p == get_current()) {
    vmm_switch_pml_root((uint64_t) p->mm->pml_root);
    if (old_mm && old_mm != &init_mm) mm_destroy(old_mm);
  }

  // Setup the Ring 3 context at the top of the kernel stack
  uint8_t *kstack_top = (uint8_t *) p->stack + (PAGE_SIZE * 4);
  cpu_regs *regs = (cpu_regs *) (kstack_top - sizeof(cpu_regs));
  memset(regs, 0, sizeof(cpu_regs));

  regs->rip = hdr->e_entry;
  regs->rsp = bprm->p;
  regs->cs = USER_CODE_SELECTOR | 3;
  regs->ss = USER_DATA_SELECTOR | 3;
  regs->rflags = 0x202; // IF=1

  // Setup return path for the new task
  uint64_t *sp = (uint64_t *) regs;
  *(--sp) = (uint64_t) ret_from_user_thread;
  *(--sp) = 0; // rbx
  *(--sp) = 0; // rbp
  *(--sp) = 0; // r12
  *(--sp) = 0; // r13
  *(--sp) = 0; // r14
  *(--sp) = 0; // r15

  p->thread.rsp = (uint64_t) sp;

  return 0;

bad_free:
  mm_destroy(bprm->mm);
  return -ENOMEM;
}

/*
 * do_execve_file - Backend for path-based execve.
 */
int do_execve_file(struct file *file, const char *name, char **argv, char **envp) {
  struct linux_binprm bprm;
  int retval;

  memset(&bprm, 0, sizeof(bprm));

  /* Read ELF header into buffer for verification */
  vfs_loff_t pos = 0;
  if (vfs_read(file, bprm.buf, sizeof(bprm.buf), &pos) < (ssize_t) sizeof(Elf64_Ehdr)) {
    return -EIO;
  }

  bprm.data = bprm.buf;
  bprm.data_len = file->f_inode->i_size;
  bprm.file = file;
  bprm.argv = argv;
  bprm.envp = envp;
  bprm.argc = 0;

  if (argv) {
    while (argv[bprm.argc]) bprm.argc++;
  }

  retval = load_elf_binary(get_current(), &bprm);
  if (retval == 0) {
    strncpy(get_current()->comm, name, 16);
  }

  return retval;
}

EXPORT_SYMBOL(do_execve_file);

/*
 * do_execve_from_buffer - The internal backend for execve from memory.
 */
int do_execve_from_buffer(void *data, size_t len, const char *name) {
  struct linux_binprm bprm;
  int retval;

  memset(&bprm, 0, sizeof(bprm));
  bprm.data = data;
  bprm.data_len = len;
  bprm.argc = 0;

  retval = load_elf_binary(get_current(), &bprm);
  if (retval == 0) {
    strncpy(get_current()->comm, name, 16);
  }

  return retval;
}
