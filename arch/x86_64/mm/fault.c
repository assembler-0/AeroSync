/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file init/main.c
 * @brief Kernel entry point and limine requests
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the VoidFrameX kernel.
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

#include <lib/string.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/exception.h>
#include <arch/x86_64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/process.h>
#include <kernel/sched/sched.h> // For current task
#include <kernel/signal.h>
#include <lib/printk.h>
#include <mm/vma.h>

// Error Code Bits
#define PF_PROT (1 << 0)    // 0: Non-present, 1: Protection violation
#define PF_WRITE (1 << 1)   // 0: Read, 1: Write
#define PF_USER (1 << 2)    // 0: Kernel, 1: User
#define PF_RSVD (1 << 3)    // 1: Reserved bit set
#define PF_INSTR (1 << 4)   // 1: Instruction fetch

void do_page_fault(cpu_regs *regs) {
  uint64_t cr2;
  __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

  uint64_t error_code = regs->error_code;
  bool user_mode = (error_code & PF_USER) || (regs->cs & 3);

  struct task_struct *curr = current;
  struct mm_struct *mm = NULL;

  if (curr) {
    // For user-space faults, we must have a mm.
    // For kernel faults, we might be using an active_mm (borrowed).
    mm = curr->mm ? curr->mm : curr->active_mm;
  }

  // Fallback to kernel's init_mm if no task context (early boot smoke tests)
  if (!mm && !user_mode) {
    mm = &init_mm;
  }

  // Security: If user mode access to higher half or canonical hole occurs, it's a SEGV.
  if (user_mode && cr2 >= vmm_get_max_user_address()) {
    printk(KERN_ERR FAULT_CLASS "User-mode access to kernel address %llx\n", cr2);
    goto signal_segv;
  }

  // If we have no MM context, this is a fatal kernel fault.
  if (!mm) {
    goto kernel_panic;
  }

  // Check for kernel-mode fault recovery (exception table)
  if (!user_mode) {
    uint64_t fixup = search_exception_table(regs->rip);
    if (fixup) {
      regs->rip = fixup;
      return;
    }
  }

  // Try to find VMA covering this address
  down_read(&mm->mmap_lock);
  struct vm_area_struct *vma = vma_find(mm, cr2);

  if (vma && cr2 >= vma->vm_start && cr2 < vma->vm_end) {
    // Valid VMA found. Check permissions.
    bool write_fault = (error_code & PF_WRITE);
    bool exec_fault = (error_code & PF_INSTR);

    if (write_fault && !(vma->vm_flags & VM_WRITE)) {
      up_read(&mm->mmap_lock);
      printk(KERN_ERR FAULT_CLASS "Page Fault: Write violation at %llx\n", cr2);
      goto signal_segv;
    }
    if (exec_fault && !(vma->vm_flags & VM_EXEC)) {
      up_read(&mm->mmap_lock);
      printk(KERN_ERR FAULT_CLASS "Page Fault: Exec violation at %llx\n", cr2);
      goto signal_segv;
    }

    // Dispatches to VMA-specific fault handler (e.g. shadow_obj_fault)
    unsigned int fault_flags = 0;
    if (write_fault) fault_flags |= FAULT_FLAG_WRITE;
    if (user_mode) fault_flags |= FAULT_FLAG_USER;
    if (exec_fault) fault_flags |= FAULT_FLAG_INSTR;

    int res = handle_mm_fault(vma, cr2, fault_flags);
    if (res == 0) {
        up_read(&mm->mmap_lock);
        return;
    }

    // Legacy/PTE COW Handling: If it's a write fault on a present page in a writable VMA
    // This is a fallback for kernel/modules or VMAs not yet using vm_objects fully.
    if (write_fault && (error_code & PF_PROT)) {
      if (vmm_handle_cow(mm, cr2) == 0) {
        up_read(&mm->mmap_lock);
        return; // Success, retry write
      }
    }

    up_read(&mm->mmap_lock);
    if (res == VM_FAULT_OOM) {
        printk(KERN_ERR FAULT_CLASS "OOM during fault handling for %llx\n", cr2);
        goto kernel_panic;
    }
    goto signal_segv;
  }
  up_read(&mm->mmap_lock);

signal_segv:
  if (user_mode) {
    printk(KERN_ERR FAULT_CLASS "segmentation fault at %llx (User)\n", cr2);
    send_signal(SIGSEGV, current);
    return;
  }

kernel_panic:
  printk(KERN_EMERG FAULT_CLASS "kernel fault at %llx\n", cr2);
  panic_exception(regs);
}
