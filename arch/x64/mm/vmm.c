/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/mm/vmm.c
 * @brief Virtual Memory Manager implementation
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
#include <arch/x64/mm/paging.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/vma.h>

// Global kernel PML4 (physical address)
uint64_t g_kernel_pml4 = 0;

static spinlock_t vmm_lock = 0;

extern char _text_start[];
extern char _text_end[];
extern char _rodata_start[];
extern char _rodata_end[];
extern char _data_start[];
extern char _data_end[];

// Helper to access physical memory using the HHDM
static inline void *phys_to_virt(uint64_t phys) {
  return pmm_phys_to_virt(phys);
}

// Helper to allocate a zeroed page table frame
static uint64_t vmm_alloc_table(void) {
  uint64_t phys = pmm_alloc_page();
  if (phys) {
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
  }
  return phys;
}

static uint64_t *get_next_level(uint64_t *current_table, uint64_t index,
                                bool alloc, int level) {
  uint64_t entry = current_table[index];

  if (entry & PTE_PRESENT) {
    if (entry & PTE_HUGE) {
      if (!alloc) {
        return NULL;
      }
      if (level != 2) {
        // We only support splitting 2MB pages (Level 2) for now.
        return NULL;
      }

      // Split 2MB Huge Page into 4KB Pages
      uint64_t new_table_phys = vmm_alloc_table();
      if (!new_table_phys)
        return NULL;

      uint64_t *new_table = (uint64_t *)phys_to_virt(new_table_phys);
      uint64_t huge_base = PTE_GET_ADDR(entry);
      uint64_t huge_flags = PTE_GET_FLAGS(entry) & ~PTE_HUGE;

      for (int i = 0; i < 512; i++) {
        new_table[i] = (huge_base + i * PAGE_SIZE) | huge_flags;
      }

      // Update the directory entry to point to the new table
      // Ensure we have Present | ReadWrite | User so that the PT entries
      // control permissions
      current_table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;

      return new_table;
    }
    return (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  }

  if (!alloc) {
    return NULL;
  }

  uint64_t new_table_phys = vmm_alloc_table();
  if (!new_table_phys) {
    return NULL;
  }

  current_table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  return (uint64_t *)phys_to_virt(new_table_phys);
}

// --- Internal Unlocked Helpers ---

static int vmm_map_page_locked(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                               uint64_t flags) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), true, 4);
  if (!pdpt)
    return -1;

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), true, 3);
  if (!pd)
    return -1;

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), true, 2);
  if (!pt)
    return -1;

  uint64_t pt_index = PT_INDEX(virt);
  pt[pt_index] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK);

  // Invalidate TLB if we are modifying the current address space
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & PTE_ADDR_MASK) == pml4_phys) {
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
  }

  return 0;
}

static int vmm_unmap_page_locked(uint64_t pml4_phys, uint64_t virt) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), false, 4);
  if (!pdpt)
    return 0;

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), false, 3);
  if (!pd)
    return 0;

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), false, 2);
  if (!pt)
    return 0;

  uint64_t pt_index = PT_INDEX(virt);
  pt[pt_index] = 0; // Clear entry

  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & PTE_ADDR_MASK) == pml4_phys) {
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
  }
  return 0;
}

// --- Public VMM API (Locked) ---

int vmm_map_page(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                 uint64_t flags) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  int ret = vmm_map_page_locked(pml4_phys, virt, phys, flags);
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return ret;
}

int vmm_unmap_page(uint64_t pml4_phys, uint64_t virt) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  int ret = vmm_unmap_page_locked(pml4_phys, virt);
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return ret;
}

// Improved virt_to_phys that handles Huge Pages
uint64_t vmm_virt_to_phys(uint64_t pml4_phys, uint64_t virt) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  uint64_t idx;
  uint64_t entry;

  // PML4
  idx = PML4_INDEX(virt);
  entry = pml4[idx];
  if (!(entry & PTE_PRESENT))
    return 0;

  // PDPT
  uint64_t *pdpt = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  idx = PDPT_INDEX(virt);
  entry = pdpt[idx];
  if (!(entry & PTE_PRESENT))
    return 0;
  if (entry & PTE_HUGE) {
    // 1GB Page
    return PTE_GET_ADDR(entry) + (virt & 0x3FFFFFFF);
  }

  // PD
  uint64_t *pd = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  idx = PD_INDEX(virt);
  entry = pd[idx];
  if (!(entry & PTE_PRESENT))
    return 0;
  if (entry & PTE_HUGE) {
    // 2MB Page
    return PTE_GET_ADDR(entry) + (virt & 0x1FFFFF);
  }

  // PT
  uint64_t *pt = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  idx = PT_INDEX(virt);
  entry = pt[idx];
  if (!(entry & PTE_PRESENT))
    return 0;

  // 4KB Page
  return PTE_GET_ADDR(entry) + (virt & 0xFFF);
}

void vmm_dump_entry(uint64_t pml4_phys, uint64_t virt) {
  uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
  printk(VMM_CLASS "Dumping flags for virt: %llx\n", virt);

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), false, 4);
  if (!pdpt) {
    printk("  PDPT missing\n");
    return;
  }

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), false, 3);
  if (!pd) {
    printk("  PD missing\n");
    return;
  }

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), false, 2);
  if (!pt) {
    printk("  PT missing\n");
    return;
  }

  uint64_t entry = pt[PT_INDEX(virt)];
  printk(VMM_CLASS "  PTE: %llx (P:%d W:%d U:%d NX:%d)\n", entry,
         !!(entry & PTE_PRESENT), !!(entry & PTE_RW), !!(entry & PTE_USER),
         !!(entry & PTE_NX));
}

void vmm_switch_pml4(uint64_t pml4_phys) {
  __asm__ volatile("mov %0, %%cr3" ::"r"(pml4_phys) : "memory");
}

void vmm_init(void) {
  printk(VMM_CLASS "Initializing VMM...\n");

  // Allocate a new PML4 for the kernel
  g_kernel_pml4 = vmm_alloc_table();
  if (!g_kernel_pml4) {
    panic(VMM_CLASS "Failed to allocate kernel PML4");
  }

  printk(VMM_CLASS "Kernel PML4 allocated at 0x%llx\n", g_kernel_pml4);

  // We need to copy the existing mappings from the bootloader's page table
  // typically the higher half (kernel, HHDM) so we don't crash immediately
  // upon switching.
  // Limine provides the current CR3.
  uint64_t boot_pml4_phys;
  __asm__ volatile("mov %%cr3, %0" : "=r"(boot_pml4_phys));
  boot_pml4_phys &= PTE_ADDR_MASK;

  uint64_t *boot_pml4 = (uint64_t *)phys_to_virt(boot_pml4_phys);
  uint64_t *kernel_pml4 = (uint64_t *)phys_to_virt(g_kernel_pml4);

  // Copy higher half (entries 256-511)
  memcpy(kernel_pml4 + 256, boot_pml4 + 256, 256 * sizeof(uint64_t));

  // Reload CR3
  vmm_switch_pml4(g_kernel_pml4);

  // Initialize kernel mm_struct
  mm_init(&init_mm);
  init_mm.pml4 = (uint64_t *)g_kernel_pml4;

  printk(VMM_CLASS "VMM Initialized and switched to new Page Table.\n");
}
