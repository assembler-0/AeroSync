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

#include <arch/x64/cpu.h>
#include <arch/x64/exception.h>
#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/sched/sched.h> // For current task
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

    uint64_t pml4_phys = (uint64_t) mm->pml4;

    // COW Handling: If it's a write fault on a present page in a writable VMA
    if (write_fault && (error_code & PF_PROT)) {
      if (vmm_handle_cow(pml4_phys, cr2) == 0) {
        up_read(&mm->mmap_lock);
        return; // Success, retry write
      }
      // If COW failed, treat as SEGV or panic
      up_read(&mm->mmap_lock);
      goto signal_segv;
    }

    // Demand Paging: Allocate a physical page if it's not present
    if (!(error_code & PF_PROT)) {
      uint64_t phys;
      uint64_t flags = PTE_PRESENT;

      if (vma->vm_flags & VM_USER) flags |= PTE_USER;
      if (vma->vm_flags & VM_WRITE) flags |= PTE_RW;
      if (!(vma->vm_flags & VM_EXEC)) flags |= PTE_NX;

      if (vma->vm_flags & VM_HUGE) {
        // Try to allocate 2MB page (Order 9: 2^9 * 4KB = 2MB)
        phys = pmm_alloc_pages(9);
        if (phys) {
          // Align virtual address to 2MB boundary
          uint64_t huge_virt = cr2 & ~(VMM_PAGE_SIZE_2M - 1);

          if (vmm_map_huge_page(pml4_phys, huge_virt, phys, flags, VMM_PAGE_SIZE_2M) == 0) {
            up_read(&mm->mmap_lock);
            return;
          }
          // If mapping failed, free pages and maybe fallback?
          pmm_free_pages(phys, 9);
        }
        // Fallback to 4KB if huge alloc failed
        // (or handle OOM if strict huge usage is required, but standard behavior implies opportunistic)
      }

      phys = pmm_alloc_page();
      if (!phys) {
        up_read(&mm->mmap_lock);
        printk(KERN_ERR FAULT_CLASS "OOM during demand paging for %llx\n", cr2);
        goto kernel_panic;
      }

      vmm_map_page(pml4_phys, cr2 & PAGE_MASK, phys, flags);
      up_read(&mm->mmap_lock);
      return; // Retry instruction
    }
  }
  up_read(&mm->mmap_lock);

signal_segv:
  if (user_mode) {
    printk(KERN_ERR FAULT_CLASS "Segmentation Fault at %llx (User)\n", cr2);
    // TODO: sys_exit or signal
    // For now, just panic/kill to be safe
    panic_exception(regs);
    return;
  }

kernel_panic:
  printk(KERN_EMERG FAULT_CLASS "Kernel Page Fault at %llx\n", cr2);
  panic_exception(regs);
}
