/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sched/elf_loader.c
 * @brief Advanced ELF binary loader with PIE and Interpreter support
 * @copyright (C) 2025-2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
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
#include <aerosync/classes.h>
#include <aerosync/crypto.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <lib/printk.h>
#include <mm/slub.h>
#include <mm/vma.h>

#define ELF_ET_DYN_BASE 0x400000000000UL
#define STACK_TOP_MAX   0x7FFFFFFFF000UL
#define STACK_SIZE      (1024 * 1024) // 1MB stack initial

struct linux_binprm {
  char buf[128];
  struct file *file;
  int argc, envc;
  char **argv, **envp;
  struct mm_struct *mm;
  uint64_t p;             /* Current user stack pointer */
  uint64_t entry;         /* Executable entry point */
  uint64_t load_addr;     /* Base load address (bias) */
  uint64_t interp_load;   /* Interpreter load address */
  uint64_t interp_entry;  /* Interpreter entry point */
  char *interp_name;      /* Name of the interpreter */
  Elf64_Phdr *phdr_addr;  /* User address of phdrs */
  uint16_t phnum;
};

static inline int elf_check_arch(Elf64_Ehdr *hdr) {
  return hdr->e_machine == EM_X86_64 && hdr->e_ident[EI_CLASS] == 2;
}

/**
 * push_stack - Push data onto the user stack.
 * @bprm: Binary parameters
 * @data: Data to push
 * @len: Length of data
 * @return: User address of pushed data
 */
static uint64_t push_stack(struct linux_binprm *bprm, const void *data, size_t len) {
  bprm->p -= len;
  uint64_t phys = vmm_virt_to_phys(bprm->mm, bprm->p);
  void *dst = pmm_phys_to_virt(phys);
  memcpy(dst, data, len);
  return bprm->p;
}

static uint64_t push_long(struct linux_binprm *bprm, uint64_t val) {
  bprm->p -= sizeof(uint64_t);
  uint64_t phys = vmm_virt_to_phys(bprm->mm, bprm->p);
  uint64_t *dst = (uint64_t *)pmm_phys_to_virt(phys);
  *dst = val;
  return bprm->p;
}

static int setup_arg_pages(struct linux_binprm *bprm) {
  uint64_t stack_top = STACK_TOP_MAX;
  uint64_t stack_base = stack_top - STACK_SIZE;

  if (mm_populate_user_range(bprm->mm, stack_base, STACK_SIZE, VM_READ | VM_WRITE | VM_USER | VM_STACK, nullptr, 0) != 0) {
    return -ENOMEM;
  }

  bprm->mm->start_stack = stack_top;
  bprm->p = stack_top;
  return 0;
}

/**
 * create_elf_tables - Setup argc, argv, envp and AuxV on user stack.
 */
static int create_elf_tables(struct linux_binprm *bprm, Elf64_Ehdr *exec) {
  (void)exec;
  uint64_t random_bytes[2];
  
  /* Use software RNG from unified crypto stack for better performance */
  struct crypto_tfm *tfm = crypto_alloc_tfm("sw_rng", CRYPTO_ALG_TYPE_RNG);
  if (tfm) {
    crypto_rng_generate(tfm, (uint8_t *)random_bytes, sizeof(random_bytes));
    crypto_free_tfm(tfm);
  } else {
    /* Fallback if RNG allocation fails (unlikely) */
    random_bytes[0] = 0xDEADC0DEBABECAFEULL;
    random_bytes[1] = 0x123456789ABCDEF0ULL;
  }
  
  uint64_t u_random = push_stack(bprm, random_bytes, 16);

  char **k_argv = kmalloc(sizeof(char *) * (bprm->argc + 1));
  char **k_envp = kmalloc(sizeof(char *) * (bprm->envc + 1));

  if (bprm->envp) {
    for (int i = bprm->envc - 1; i >= 0; i--) {
      size_t len = strlen(bprm->envp[i]) + 1;
      k_envp[i] = (char *)push_stack(bprm, bprm->envp[i], len);
    }
  }
  
  if (bprm->argv) {
    for (int i = bprm->argc - 1; i >= 0; i--) {
      size_t len = strlen(bprm->argv[i]) + 1;
      k_argv[i] = (char *)push_stack(bprm, bprm->argv[i], len);
    }
  }

  bprm->p &= ~15;

  push_long(bprm, 0); push_long(bprm, AT_NULL);
  push_long(bprm, bprm->entry); push_long(bprm, AT_ENTRY);
  push_long(bprm, bprm->phnum); push_long(bprm, AT_PHNUM);
  push_long(bprm, sizeof(Elf64_Phdr)); push_long(bprm, AT_PHENT);
  push_long(bprm, (uint64_t)bprm->phdr_addr); push_long(bprm, AT_PHDR);
  push_long(bprm, PAGE_SIZE); push_long(bprm, AT_PAGESZ);
  push_long(bprm, u_random); push_long(bprm, AT_RANDOM);
  push_long(bprm, bprm->interp_load); push_long(bprm, AT_BASE);
  push_long(bprm, 0); push_long(bprm, AT_FLAGS);
  push_long(bprm, 1000); push_long(bprm, AT_UID);
  push_long(bprm, 1000); push_long(bprm, AT_GID);

  push_long(bprm, 0);
  if (bprm->envp) {
    for (int i = bprm->envc - 1; i >= 0; i--) push_long(bprm, (uint64_t)k_envp[i]);
  }

  push_long(bprm, 0);
  if (bprm->argv) {
    for (int i = bprm->argc - 1; i >= 0; i--) push_long(bprm, (uint64_t)k_argv[i]);
  }

  push_long(bprm, bprm->argc);

  kfree(k_argv);
  kfree(k_envp);
  return 0;
}

static int load_elf_interp(struct linux_binprm *bprm, const char *path) {
  struct file *file = vfs_open(path, O_RDONLY, 0);
  if (!file) {
    printk(KERN_ERR KERN_CLASS "Failed to open interpreter %s\n", path);
    return -ENOENT;
  }

  Elf64_Ehdr hdr;
  vfs_loff_t pos = 0;
  if (kernel_read(file, &hdr, sizeof(hdr), &pos) != sizeof(hdr)) {
    vfs_close(file);
    return -EIO;
  }

  if (memcmp(hdr.e_ident, ELFMAG, SELFMAG) != 0 || hdr.e_type != ET_DYN) {
    vfs_close(file);
    return -ENOEXEC;
  }

  size_t phdr_size = hdr.e_phentsize * hdr.e_phnum;
  Elf64_Phdr *phdrs = kmalloc(phdr_size);
  pos = hdr.e_phoff;
  kernel_read(file, phdrs, phdr_size, &pos);

  uint64_t load_bias = ELF_ET_DYN_BASE + 0x1000000; 
  bprm->interp_load = load_bias;
  bprm->interp_entry = hdr.e_entry + load_bias;

  for (int i = 0; i < hdr.e_phnum; i++) {
    if (phdrs[i].p_type != PT_LOAD) continue;

    uint64_t vaddr = phdrs[i].p_vaddr + load_bias;
    uint64_t prot = PROT_READ;
    if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
    if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;

    uint64_t mmap_flags = MAP_PRIVATE | MAP_FIXED;
    do_mmap(bprm->mm, vaddr, phdrs[i].p_memsz, prot, mmap_flags, 
            file, nullptr, phdrs[i].p_offset >> PAGE_SHIFT);
  }

  kfree(phdrs);
  vfs_close(file);
  return 0;
}

static int load_elf_binary(struct task_struct *p, struct linux_binprm *bprm) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *) bprm->buf;
  Elf64_Phdr *phdrs;
  int retval;

  if (memcmp(hdr->e_ident, ELFMAG, SELFMAG) != 0 || !elf_check_arch(hdr))
    return -ENOEXEC;

  if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
    return -ENOEXEC;

  bprm->mm = mm_create();
  if (!bprm->mm) return -ENOMEM;

  size_t phdr_size = hdr->e_phentsize * hdr->e_phnum;
  phdrs = kmalloc(phdr_size);
  if (!phdrs) {
    retval = -ENOMEM;
    goto bad_mm;
  }

  vfs_loff_t pos = hdr->e_phoff;
  if (kernel_read(bprm->file, phdrs, phdr_size, &pos) != (ssize_t)phdr_size) {
    retval = -EIO;
    goto bad_free_ph;
  }

  if (hdr->e_type == ET_DYN) {
    bprm->load_addr = ELF_ET_DYN_BASE;
  }

  for (int i = 0; i < hdr->e_phnum; i++) {
    if (phdrs[i].p_type == PT_INTERP) {
      char *interp = kmalloc(phdrs[i].p_filesz + 1);
      pos = phdrs[i].p_offset;
      kernel_read(bprm->file, interp, phdrs[i].p_filesz, &pos);
      interp[phdrs[i].p_filesz] = '\0';
      bprm->interp_name = interp;
    }

    if (phdrs[i].p_type != PT_LOAD) continue;

    uint64_t vaddr = phdrs[i].p_vaddr + bprm->load_addr;
    uint64_t prot = PROT_READ;
    if (phdrs[i].p_flags & PF_W) prot |= PROT_WRITE;
    if (phdrs[i].p_flags & PF_X) prot |= PROT_EXEC;

    uint64_t mmap_flags = MAP_PRIVATE | MAP_FIXED;
    uint64_t ret = do_mmap(bprm->mm, vaddr, phdrs[i].p_memsz, prot, mmap_flags, 
                          bprm->file, nullptr, phdrs[i].p_offset >> PAGE_SHIFT);
    if (ret != vaddr) {
      retval = -ENOMEM;
      goto bad_free_ph;
    }

    if (phdrs[i].p_offset == 0) {
      bprm->phdr_addr = (Elf64_Phdr *)(vaddr + hdr->e_phoff);
    }
  }

  bprm->entry = hdr->e_entry + bprm->load_addr;
  bprm->phnum = hdr->e_phnum;

  retval = setup_arg_pages(bprm);
  if (retval < 0) goto bad_free_ph;

  if (bprm->interp_name) {
    retval = load_elf_interp(bprm, bprm->interp_name);
    if (retval < 0) goto bad_free_ph;
    kfree(bprm->interp_name);
    bprm->interp_name = nullptr;
  }

  create_elf_tables(bprm, hdr);

  struct mm_struct *old_mm = p->mm;
  p->mm = bprm->mm;
  p->active_mm = bprm->mm;
  p->flags &= ~PF_KTHREAD;

  if (p == get_current()) {
    vmm_switch_pml_root((uint64_t) p->mm->pml_root);
    if (old_mm && old_mm != &init_mm) mm_destroy(old_mm);
  }

  uint8_t *kstack_top = (uint8_t *) p->stack + (PAGE_SIZE * 4);
  cpu_regs *regs = (cpu_regs *) (kstack_top - sizeof(cpu_regs));
  memset(regs, 0, sizeof(cpu_regs));

  regs->rip = bprm->interp_load ? bprm->interp_entry : bprm->entry;
  regs->rsp = bprm->p;
  regs->cs = USER_CODE_SELECTOR | 3;
  regs->ss = USER_DATA_SELECTOR | 3;
  regs->rflags = 0x202; 

  kfree(phdrs);
  return 0;

bad_free_ph:
  kfree(phdrs);
  if (bprm->interp_name) kfree(bprm->interp_name);
bad_mm:
  mm_destroy(bprm->mm);
  return retval;
}

int do_execve_file(struct file *file, const char *name, char **argv, char **envp) {
  struct linux_binprm bprm;
  memset(&bprm, 0, sizeof(bprm));

  vfs_loff_t pos = 0;
  if (kernel_read(file, bprm.buf, sizeof(bprm.buf), &pos) < (ssize_t) sizeof(Elf64_Ehdr)) {
    return -EIO;
  }

  bprm.file = file;
  bprm.argv = argv;
  bprm.envp = envp;
  
  if (argv) {
    while (argv[bprm.argc]) bprm.argc++;
  }
  if (envp) {
    while (envp[bprm.envc]) bprm.envc++;
  }

  int retval = load_elf_binary(get_current(), &bprm);
  if (retval == 0) {
    strncpy(get_current()->comm, name, 16);
  }

  return retval;
}

EXPORT_SYMBOL(do_execve_file);

int do_execve_from_buffer(void *data, size_t len, const char *name) {
  (void)data; (void)len; (void)name;
  return -ENOSYS; 
}
