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
#include <lib/string.h>
#include <mm/vma.h>

struct linux_binprm {
    char buf[128];
    void *data;
    size_t data_len;
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

    if (mm_populate_user_range(bprm->mm, stack_base, stack_size, VM_READ | VM_WRITE | VM_USER, NULL, 0) != 0) {
        return -ENOMEM;
    }

    bprm->mm->start_stack = stack_top;
    bprm->p = stack_top;
    return 0;
}

/*
 * create_elf_tables - Setup argc, argv on the user stack.
 * For now, this is a simplified version.
 */
static int create_elf_tables(struct linux_binprm *bprm, Elf64_Ehdr *exec) {
    // In a real kernel, we would copy argv strings to the stack and then 
    // setup the pointers. For now, let's just push argc = 0 and NULL.
    
    uint64_t *stack = (uint64_t *)pmm_phys_to_virt(vmm_virt_to_phys(bprm->mm, bprm->p - 16));
    // Note: This is hacky because we access via HHDM.
    
    // Push argc, argv[0], NULL
    bprm->p -= 24;
    uint64_t *sp = (uint64_t *)((uint8_t *)stack + (bprm->p % PAGE_SIZE));
    sp[0] = bprm->argc;
    sp[1] = 0; // argv[0]
    sp[2] = 0; // NULL
    
    return 0;
}

static int load_elf_binary(struct task_struct *p, struct linux_binprm *bprm) {
    Elf64_Ehdr *hdr = (Elf64_Ehdr *)bprm->data;
    Elf64_Phdr *phdrs;
    int retval;

    if (hdr->e_ident[EI_MAG0] != ELFMAG0 || hdr->e_ident[EI_MAG1] != ELFMAG1)
        return -ENOEXEC;

    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN)
        return -ENOEXEC;

    bprm->mm = mm_create();
    if (!bprm->mm) return -ENOMEM;

    phdrs = (Elf64_Phdr *)((uint8_t *)bprm->data + hdr->e_phoff);

    for (int i = 0; i < hdr->e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint64_t vaddr = phdrs[i].p_vaddr;
        uint64_t filesz = phdrs[i].p_filesz;
        uint64_t memsz = phdrs[i].p_memsz;
        uint64_t offset = phdrs[i].p_offset;
        uint64_t flags = VM_READ | VM_USER;
        
        if (phdrs[i].p_flags & PF_W) flags |= VM_WRITE;
        if (phdrs[i].p_flags & PF_X) flags |= VM_EXEC;

        // Map and populate the segment
        if (mm_populate_user_range(bprm->mm, vaddr, memsz, flags, 
                                   (uint8_t *)bprm->data + offset, filesz) != 0) {
            goto bad_free;
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
        vmm_switch_pml_root((uint64_t)p->mm->pml_root);
        if (old_mm && old_mm != &init_mm) mm_destroy(old_mm);
    }

    // Setup the Ring 3 context at the top of the kernel stack
    uint8_t *kstack_top = (uint8_t *)p->stack + (PAGE_SIZE * 4);
    cpu_regs *regs = (cpu_regs *)(kstack_top - sizeof(cpu_regs));
    memset(regs, 0, sizeof(cpu_regs));
    
    regs->rip = hdr->e_entry;
    regs->rsp = bprm->p;
    regs->cs = USER_CODE_SELECTOR | 3;
    regs->ss = USER_DATA_SELECTOR | 3;
    regs->rflags = 0x202; // IF=1

    // Setup return path for the new task
    uint64_t *sp = (uint64_t *)regs;
    *(--sp) = (uint64_t)ret_from_user_thread;
    *(--sp) = 0; // rbx
    *(--sp) = 0; // rbp
    *(--sp) = 0; // r12
    *(--sp) = 0; // r13
    *(--sp) = 0; // r14
    *(--sp) = 0; // r15
    
    p->thread.rsp = (uint64_t)sp;
    
    return 0;

bad_free:
    mm_destroy(bprm->mm);
    return -ENOMEM;
}

/*
 * do_execve_from_buffer - The internal backend for execve.
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