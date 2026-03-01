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

#include <aerosync/classes.h>
#include <aerosync/crypto.h>
#include <aerosync/elf.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/gdt/gdt.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <mm/slub.h>
#include <mm/vma.h>

#define ELF_ET_DYN_BASE 0x400000000000UL
#define STACK_TOP_MAX 0x7FFFFFFFF000UL
#define STACK_SIZE (1024 * 1024) // 1MB stack initial

struct linux_binprm {
  char buf[128];
  struct file *file;
  int argc, envc;
  char **argv, **envp;
  struct mm_struct *mm;
  uint64_t p;            /* Current user stack pointer */
  uint64_t entry;        /* Executable entry point */
  uint64_t load_addr;    /* Base load address (bias) */
  uint64_t interp_load;  /* Interpreter load address */
  uint64_t interp_entry; /* Interpreter entry point */
  char *interp_name;     /* Name of the interpreter */
  Elf64_Phdr *phdr_addr; /* User address of phdrs */
  uint16_t phnum;
};

static inline int elf_check_arch(Elf64_Ehdr *hdr) {
  return hdr->e_machine == EM_X86_64 && hdr->e_ident[EI_CLASS] == 2;
}

/**
 * write_user_hhdm - Write to user memory via HHDM without context switching.
 */
static void write_user_hhdm(struct mm_struct *mm, uint64_t vaddr,
                            const void *data, size_t len) {
  const uint8_t *src = (const uint8_t *)data;
  while (len > 0) {
    uint64_t phys = vmm_virt_to_phys(mm, vaddr);
    if (!phys) {
      printk(KERN_DEBUG VMM_CLASS "populating page for %llx\n", vaddr);
      mm_populate_range(mm, vaddr & PAGE_MASK, (vaddr & PAGE_MASK) + PAGE_SIZE,
                        false);
      phys = vmm_virt_to_phys(mm, vaddr);
      if (!phys) {
        printk(KERN_ERR VMM_CLASS "failed to populate page for %llx\n", vaddr);
        return;
      }
    }

    uint64_t page_off = vaddr & (PAGE_SIZE - 1);
    size_t to_write = PAGE_SIZE - page_off;
    if (to_write > len)
      to_write = len;

    void *dst = pmm_phys_to_virt(phys);
    memcpy(dst, src, to_write);

    vaddr += to_write;
    src += to_write;
    len -= to_write;
  }
}

/**
 * memset_user_hhdm - Zero user memory via HHDM without context switching.
 */
static void memset_user_hhdm(struct mm_struct *mm, uint64_t vaddr, uint8_t val,
                             size_t len) {
  while (len > 0) {
    uint64_t phys = vmm_virt_to_phys(mm, vaddr);
    if (!phys) {
      printk(KERN_DEBUG VMM_CLASS "populating page (memset) for %llx\n", vaddr);
      mm_populate_range(mm, vaddr & PAGE_MASK, (vaddr & PAGE_MASK) + PAGE_SIZE,
                        false);
      phys = vmm_virt_to_phys(mm, vaddr);
      if (!phys) {
        printk(KERN_ERR VMM_CLASS "FAILED to populate page (memset) for %llx\n",
               vaddr);
        return;
      }
    }

    uint64_t page_off = vaddr & (PAGE_SIZE - 1);
    size_t to_set = PAGE_SIZE - page_off;
    if (to_set > len)
      to_set = len;

    void *dst = pmm_phys_to_virt(phys);
    memset(dst, val, to_set);

    vaddr += to_set;
    len -= to_set;
  }
}

static uint64_t push_stack(struct linux_binprm *bprm, const void *data,
                           size_t len) {
  bprm->p -= len;
  write_user_hhdm(bprm->mm, bprm->p, data, len);
  return bprm->p;
}

static uint64_t push_long(struct linux_binprm *bprm, uint64_t val) {
  bprm->p -= sizeof(uint64_t);
  write_user_hhdm(bprm->mm, bprm->p, &val, sizeof(uint64_t));
  return bprm->p;
}

static int setup_arg_pages(struct linux_binprm *bprm) {
  uint64_t stack_top = STACK_TOP_MAX;
  uint64_t stack_base = stack_top - STACK_SIZE;

  if (mm_populate_user_range(bprm->mm, stack_base, STACK_SIZE,
                             VM_READ | VM_WRITE | VM_USER | VM_STACK, nullptr,
                             0) != 0) {
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

  push_long(bprm, 0);
  push_long(bprm, AT_NULL);
  push_long(bprm, bprm->entry);
  push_long(bprm, AT_ENTRY);
  push_long(bprm, bprm->phnum);
  push_long(bprm, AT_PHNUM);
  push_long(bprm, sizeof(Elf64_Phdr));
  push_long(bprm, AT_PHENT);
  push_long(bprm, (uint64_t)bprm->phdr_addr);
  push_long(bprm, AT_PHDR);
  push_long(bprm, PAGE_SIZE);
  push_long(bprm, AT_PAGESZ);
  push_long(bprm, u_random);
  push_long(bprm, AT_RANDOM);
  push_long(bprm, bprm->interp_load);
  push_long(bprm, AT_BASE);
  push_long(bprm, 0);
  push_long(bprm, AT_FLAGS);
  push_long(bprm, 1000);
  push_long(bprm, AT_UID);
  push_long(bprm, 1000);
  push_long(bprm, AT_GID);

  push_long(bprm, 0);
  if (bprm->envp) {
    for (int i = bprm->envc - 1; i >= 0; i--)
      push_long(bprm, (uint64_t)k_envp[i]);
  }

  push_long(bprm, 0);
  if (bprm->argv) {
    for (int i = bprm->argc - 1; i >= 0; i--)
      push_long(bprm, (uint64_t)k_argv[i]);
  }

  push_long(bprm, bprm->argc);

  kfree(k_argv);
  kfree(k_envp);
  return 0;
}

static int load_elf_interp(struct linux_binprm *bprm, const char *path) {
  printk(KERN_DEBUG ELF_CLASS "loading interpreter: %s\n", path);
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
    if (phdrs[i].p_type != PT_LOAD)
      continue;

    uint64_t vaddr = phdrs[i].p_vaddr + load_bias;
    uint64_t prot = PROT_READ;
    if (phdrs[i].p_flags & PF_W)
      prot |= PROT_WRITE;
    if (phdrs[i].p_flags & PF_X)
      prot |= PROT_EXEC;

    printk(KERN_DEBUG ELF_CLASS "phdr[%d]: vaddr=%llx p_offset=%llx\n", i,
           phdrs[i].p_vaddr, phdrs[i].p_offset);

    uint64_t align_diff = vaddr & (PAGE_SIZE - 1);
    uint64_t base_vaddr = vaddr & ~(PAGE_SIZE - 1);
    uint64_t base_offset = phdrs[i].p_offset & ~(PAGE_SIZE - 1);

    uint64_t mmap_flags = MAP_PRIVATE | MAP_FIXED;
    if (phdrs[i].p_filesz > 0 || align_diff > 0) {
      uint64_t map_len = phdrs[i].p_filesz + align_diff;
      do_mmap(bprm->mm, base_vaddr, map_len, prot, mmap_flags, file, nullptr,
              base_offset >> PAGE_SHIFT);
    }

    uint64_t bss_start = vaddr + phdrs[i].p_filesz;
    uint64_t bss_end = vaddr + phdrs[i].p_memsz;
    uint64_t page_end = (bss_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (bss_end > bss_start) {
      if (page_end > bss_start) {
        size_t zlen = page_end - bss_start;
        if (zlen > (bss_end - bss_start))
          zlen = bss_end - bss_start;

        memset_user_hhdm(bprm->mm, bss_start, 0, zlen);
      }

      if (bss_end > page_end) {
        uint64_t extra_len = bss_end - page_end;
        do_mmap(bprm->mm, page_end, extra_len, prot,
                MAP_PRIVATE | MAP_FIXED | MAP_ANON, nullptr, nullptr, 0);
      }
    }
  }

  kfree(phdrs);
  vfs_close(file);
  return 0;
}

static int load_elf_binary(struct task_struct *p, struct linux_binprm *bprm) {
  Elf64_Ehdr *hdr = (Elf64_Ehdr *)bprm->buf;
  Elf64_Phdr *phdrs;
  int retval;

  printk(
      KERN_DEBUG
      ELF_CLASS "load_elf_binary starting for %p (phnum=%d, entry=%llx, type=%d)\n",
      p, hdr->e_phnum, hdr->e_entry, hdr->e_type);

  if (memcmp(hdr->e_ident, ELFMAG, SELFMAG) != 0 || !elf_check_arch(hdr))
    return -ENOEXEC;

  if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
    return -ENOEXEC;

  bprm->mm = mm_create();
  if (!bprm->mm)
    return -ENOMEM;

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
    printk(KERN_DEBUG ELF_CLASS "examining phdr %d of %d (type=%u)\n", i,
           hdr->e_phnum, phdrs[i].p_type);
    if (phdrs[i].p_type == PT_INTERP) {
      char *interp = kmalloc(phdrs[i].p_filesz + 1);
      pos = phdrs[i].p_offset;
      kernel_read(bprm->file, interp, phdrs[i].p_filesz, &pos);
      interp[phdrs[i].p_filesz] = '\0';
      bprm->interp_name = interp;
      printk(KERN_DEBUG ELF_CLASS "found interpreter: %s\n", interp);
    }

    if (phdrs[i].p_type != PT_LOAD) {
      printk(KERN_DEBUG ELF_CLASS "phdr %d is not LOAD, skipping\n", i);
      continue;
    }

    uint64_t vaddr = phdrs[i].p_vaddr + bprm->load_addr;
    uint64_t prot = PROT_READ;
    if (phdrs[i].p_flags & PF_W)
      prot |= PROT_WRITE;
    if (phdrs[i].p_flags & PF_X)
      prot |= PROT_EXEC;

    uint64_t align_diff = vaddr & (PAGE_SIZE - 1);
    uint64_t base_vaddr = vaddr & ~(PAGE_SIZE - 1);
    uint64_t base_offset = phdrs[i].p_offset & ~(PAGE_SIZE - 1);

    uint64_t mmap_flags = MAP_PRIVATE | MAP_FIXED;

    printk(KERN_DEBUG
           ELF_CLASS "phdr[%d]: type=%d vaddr=%llx memsz=%llx filesz=%llx\n",
           i, phdrs[i].p_type, vaddr, phdrs[i].p_memsz, phdrs[i].p_filesz);

    if (phdrs[i].p_filesz > 0 || align_diff > 0) {
      uint64_t map_len = phdrs[i].p_filesz + align_diff;
      printk(KERN_DEBUG
             ELF_CLASS "do_mmap(vaddr=%llx, len=%llx, off=%llx) calling\n",
             base_vaddr, map_len, base_offset);
      uint64_t retVal = do_mmap(bprm->mm, base_vaddr, map_len, prot, mmap_flags,
                                bprm->file, nullptr, base_offset >> PAGE_SHIFT);
      printk(KERN_DEBUG ELF_CLASS "do_mmap returned %llx\n", retVal);
      if (retVal != base_vaddr) {
        retval = -ENOMEM;
        goto bad_free_ph;
      }
    }

    uint64_t bss_start = vaddr + phdrs[i].p_filesz;
    uint64_t bss_end = vaddr + phdrs[i].p_memsz;
    uint64_t page_end = (bss_start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (bss_end > bss_start) {
      if (page_end > bss_start) {
        size_t zlen = page_end - bss_start;
        if (zlen > (bss_end - bss_start))
          zlen = bss_end - bss_start;

        printk(KERN_DEBUG
               ELF_CLASS "memset_user_hhdm(start=%llx, len=%zu) calling\n",
               bss_start, zlen);
        memset_user_hhdm(bprm->mm, bss_start, 0, zlen);
        printk(KERN_DEBUG ELF_CLASS "memset_user_hhdm finished\n");
      }

      if (bss_end > page_end) {
        uint64_t extra_len = bss_end - page_end;
        do_mmap(bprm->mm, page_end, extra_len, prot,
                MAP_PRIVATE | MAP_FIXED | MAP_ANON, nullptr, nullptr, 0);
      }
    }
    if (phdrs[i].p_offset == 0) {
      bprm->phdr_addr = (Elf64_Phdr *)(vaddr + hdr->e_phoff);
    }
  }
  printk(KERN_DEBUG ELF_CLASS "program header loop finished\n");

  printk(KERN_DEBUG ELF_CLASS "setup_arg_pages starting\n");
  retval = setup_arg_pages(bprm);
  if (retval < 0)
    goto bad_free_ph;

  if (bprm->interp_name) {
    printk(KERN_DEBUG ELF_CLASS "loading interpreter: %s\n", bprm->interp_name);
    retval = load_elf_interp(bprm, bprm->interp_name);
    if (retval < 0)
      goto bad_free_ph;
    kfree(bprm->interp_name);
    bprm->interp_name = nullptr;
  }

  printk(KERN_DEBUG ELF_CLASS "creating elf tables\n");
  create_elf_tables(bprm, hdr);

  printk(KERN_DEBUG ELF_CLASS "final task transition\n");

  struct mm_struct *old_mm = p->mm;
  p->mm = bprm->mm;
  p->active_mm = bprm->mm;
  p->flags &= ~PF_KTHREAD;

  if (p == get_current()) {
    vmm_switch_pml_root((uint64_t)p->mm->pml_root);
    if (old_mm && old_mm != &init_mm)
      mm_destroy(old_mm);
  }

  uint8_t *kstack_top = (uint8_t *)p->stack + (PAGE_SIZE * 4);
  cpu_regs *regs = (cpu_regs *)(kstack_top - sizeof(cpu_regs));
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
  if (bprm->interp_name)
    kfree(bprm->interp_name);
bad_mm:
  mm_destroy(bprm->mm);
  return retval;
}

int do_execve_file(struct file *file, const char *name, char **argv,
                   char **envp) {
  printk(KERN_DEBUG ELF_CLASS "do_execve_file starting: %s\n", name);
  struct linux_binprm bprm;
  memset(&bprm, 0, sizeof(bprm));

  vfs_loff_t pos = 0;
  if (kernel_read(file, bprm.buf, sizeof(bprm.buf), &pos) <
      (ssize_t)sizeof(Elf64_Ehdr)) {
    return -EIO;
  }

  bprm.file = file;
  bprm.argv = argv;
  bprm.envp = envp;

  if (argv) {
    while (argv[bprm.argc])
      bprm.argc++;
  }
  if (envp) {
    while (envp[bprm.envc])
      bprm.envc++;
  }

  int retval = load_elf_binary(get_current(), &bprm);
  if (retval == 0) {
    strncpy(get_current()->comm, name, 16);
  }

  return retval;
}

EXPORT_SYMBOL(do_execve_file);

int do_execve_from_buffer(void *data, size_t len, const char *name) {
  (void)data;
  (void)len;
  (void)name;
  return -ENOSYS;
}
