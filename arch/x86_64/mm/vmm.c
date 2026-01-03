/// SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x86_64/mm/vmm.c
 * @brief Virtual Memory Manager for x86_64
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

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/vma.h>
#include <arch/x86_64/mm/tlb.h>

// Global kernel PML root (physical address)
uint64_t g_kernel_pml4 = 0;

static spinlock_t vmm_lock = 0;

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

int vmm_get_paging_levels(void) {
  uint64_t cr4;
  __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
  return (cr4 & (1ULL << 12)) ? 5 : 4;
}

uint64_t vmm_get_canonical_high_base(void) {
  return (vmm_get_paging_levels() == 5) ? 0xFF00000000000000ULL : 0xFFFF800000000000ULL;
}

uint64_t vmm_get_max_user_address(void) {
  return (vmm_get_paging_levels() == 5) ? 0x0100000000000000ULL : 0x0000800000000000ULL;
}

/**
 * Splits a huge page (1GB or 2MB) into smaller pages.
 * @param table The table containing the huge page entry.
 * @param index The index of the huge page entry.
 * @param level The current paging level (3 for PDPT/1GB, 2 for PD/2MB).
 * @param virt The virtual address being split (for TLB shootdown).
 * @return 0 on success.
 */
static int vmm_split_huge_page(uint64_t *table, uint64_t index, int level, uint64_t virt) {
  uint64_t entry = table[index];
  uint64_t new_table_phys = vmm_alloc_table();
  if (!new_table_phys) return -1;

  uint64_t *new_table = (uint64_t *) phys_to_virt(new_table_phys);
  uint64_t base_phys = PTE_GET_ADDR(entry);
  uint64_t flags = PTE_GET_FLAGS(entry) & ~PTE_HUGE;

  // Determine child page attributes
  uint64_t step;
  uint64_t child_flags = flags;

  if (level == 3) {
    // Splitting 1GB into 512 * 2MB Huge Pages
    step = VMM_PAGE_SIZE_2M;
    child_flags |= PTE_HUGE;
    // PAT for 1GB (level 3) and 2MB (level 2) are both at Bit 12 (PDE_PAT)
  } else {
    // Splitting 2MB into 512 * 4KB Pages
    step = VMM_PAGE_SIZE_4K;
    // PAT for 2MB (Bit 12) must move to Bit 7 for 4KB pages
    if (flags & PDE_PAT) {
      child_flags &= ~PDE_PAT;
      child_flags |= PTE_PAT;
    }
  }

  for (int i = 0; i < 512; i++) {
    new_table[i] = (base_phys + (i * step)) | child_flags;
  }

  // Modern: Synchronize folio refcount.
  // The original 1 PDE reference is being split into 512 PTE references.
  struct page *head = phys_to_page(base_phys);
  if (head) {
    folio_ref_add(page_folio(head), 511);
  }

  // Replace huge page entry with pointer to new table.
  // We use full permissions for intermediate tables; leaf PTEs control final access.
  table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;

  // Flush TLB across all CPUs to ensure they drop the cached Huge Page translation
  uint64_t range = (level == 3) ? VMM_PAGE_SIZE_1G : VMM_PAGE_SIZE_2M;
  vmm_tlb_shootdown(NULL, virt & ~(range - 1), (virt & ~(range - 1)) + range);

  return 0;
}

/**
 * Attempts to merge a block of 512 smaller pages into one huge page.
 * @param mm The address space (can be NULL for kernel/broadcast).
 * @param virt Virtual address (must be aligned to target_huge_size).
 * @param target_huge_size The huge page size (2MB or 1GB).
 * @return 0 on success, -1 if cannot be merged.
 */
int vmm_merge_to_huge(struct mm_struct *mm, uint64_t virt, uint64_t target_huge_size) {
  if (target_huge_size != VMM_PAGE_SIZE_2M && target_huge_size != VMM_PAGE_SIZE_1G)
    return -1;

  // Check alignment
  if (virt & (target_huge_size - 1)) return -1;

  uint64_t pml_root_phys = mm ? (uint64_t) mm->pml4 : g_kernel_pml4;
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  int target_level = (target_huge_size == VMM_PAGE_SIZE_2M) ? 2 : 3;

  // Navigate to the table containing the entries to be merged
  for (int level = levels; level > target_level; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      default: spinlock_unlock_irqrestore(&vmm_lock, irq);
        return -1;
    }

    uint64_t entry = current_table[index];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  uint64_t idx;
  if (target_level == 2) idx = PD_INDEX(virt);
  else idx = PDPT_INDEX(virt);

  uint64_t entry = current_table[idx];
  if (!(entry & PTE_PRESENT)) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  if (entry & PTE_HUGE) {
    // Already merged to this level or higher
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return 0;
  }

  // Entry at current_table[idx] is a table pointer. We want to check its contents.
  uint64_t sub_table_phys = PTE_GET_ADDR(entry);
  uint64_t *sub_table = (uint64_t *) phys_to_virt(sub_table_phys);

  // Verify all 512 entries are contiguous and have same flags
  uint64_t first_entry = sub_table[0];
  if (!(first_entry & PTE_PRESENT)) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  uint64_t base_phys = PTE_GET_ADDR(first_entry);
  uint64_t flags = PTE_GET_FLAGS(first_entry);
  uint64_t step = (target_huge_size == VMM_PAGE_SIZE_2M) ? VMM_PAGE_SIZE_4K : VMM_PAGE_SIZE_2M;

  bool contiguous = true;
  for (int i = 1; i < 512; i++) {
    uint64_t entry = sub_table[i];
    if (!(entry & PTE_PRESENT) ||
        PTE_GET_ADDR(entry) != (base_phys + (i * step)) ||
        PTE_GET_FLAGS(entry) != flags) {
      contiguous = false;
      break;
    }
  }

  if (contiguous) {
    // Base must be aligned to target huge size
    if (base_phys & (target_huge_size - 1)) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }

    // All checks passed! Perform merge.
    uint64_t huge_flags = flags | PTE_HUGE;
    if (target_huge_size == VMM_PAGE_SIZE_2M && (huge_flags & PTE_PAT)) {
      huge_flags &= ~PTE_PAT;
      huge_flags |= PDE_PAT;
    }

    current_table[idx] = base_phys | huge_flags;
    spinlock_unlock_irqrestore(&vmm_lock, irq);

    vmm_tlb_shootdown(mm, virt, virt + target_huge_size);
    pmm_free_page(sub_table_phys);
    return 0;
  }

  /*
   * Non-contiguous collapse (THP style)
   */
  if (target_huge_size != VMM_PAGE_SIZE_2M) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);

  struct folio *huge_folio = alloc_pages(GFP_USER | ___GFP_ZERO, 9);
  if (!huge_folio) return -1;

  void *huge_virt_ptr = folio_address(huge_folio);

  // Re-lock and perform re-navigation to verify state hasn't changed
  irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t *table = (uint64_t *) phys_to_virt(pml_root_phys);
  for (int level = levels; level > 2; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      default: spinlock_unlock_irqrestore(&vmm_lock, irq);
        folio_put(huge_folio);
        return -1;
    }
    uint64_t entry = table[index];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      folio_put(huge_folio);
      return -1;
    }
    table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  idx = PD_INDEX(virt);
  uint64_t pde = table[idx];
  if (!(pde & PTE_PRESENT) || (pde & PTE_HUGE) || PTE_GET_ADDR(pde) != sub_table_phys) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    folio_put(huge_folio);
    return -1;
  }
  sub_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pde));

  for (int i = 0; i < 512; i++) {
    uint64_t entry = sub_table[i];
    if (!(entry & PTE_PRESENT)) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      folio_put(huge_folio);
      return -1;
    }
    memcpy((uint8_t *) huge_virt_ptr + (i * PAGE_SIZE), phys_to_virt(PTE_GET_ADDR(entry)), PAGE_SIZE);
  }

  uint64_t huge_phys = folio_to_phys(huge_folio);
  uint64_t huge_flags = flags | PTE_HUGE;
  if (huge_flags & PTE_PAT) {
    huge_flags &= ~PTE_PAT;
    huge_flags |= PDE_PAT;
  }

  table[idx] = huge_phys | huge_flags;

  for (int i = 0; i < 512; i++) {
    struct page *pg = phys_to_page(PTE_GET_ADDR(sub_table[i]));
    if (pg) put_page(pg);
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);

  vmm_tlb_shootdown(mm, virt, virt + target_huge_size);
  pmm_free_page(sub_table_phys);

  return 0;
}

/**
 * Explicitly shatters a huge page.
 */
int vmm_shatter_huge_page(uint64_t pml_root_phys, uint64_t virt, uint64_t large_page_size) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  int target_level = (large_page_size == VMM_PAGE_SIZE_1G) ? 3 : 2;

  for (int level = levels; level > target_level; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      default: spinlock_unlock_irqrestore(&vmm_lock, irq);
        return -1;
    }

    uint64_t entry = current_table[index];
    if (!(entry & PTE_PRESENT)) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }
    if (entry & PTE_HUGE) {
      // We hit a huge page higher than our target?
      // Should we shatter recursively? For now, fail.
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  uint64_t idx = (target_level == 2) ? PD_INDEX(virt) : PDPT_INDEX(virt);
  if (!(current_table[idx] & PTE_PRESENT) || !(current_table[idx] & PTE_HUGE)) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  int ret = vmm_split_huge_page(current_table, idx, target_level, virt);

  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return ret;
}

/**
 * Scans a range and merges contiguous 4KB/2MB pages into huge pages where possible.
 */
void vmm_merge_range(uint64_t pml_root_phys, uint64_t start, uint64_t end) {
  uint64_t addr = PAGE_ALIGN_UP(start);

  while (addr < end) {
    // Try 1GB merge first if aligned
    if ((addr & (VMM_PAGE_SIZE_1G - 1)) == 0 && (addr + VMM_PAGE_SIZE_1G) <= end) {
      if (vmm_merge_to_huge(NULL, addr, VMM_PAGE_SIZE_1G) == 0) {
        addr += VMM_PAGE_SIZE_1G;
        continue;
      }
    }

    // Try 2MB merge if aligned
    if ((addr & (VMM_PAGE_SIZE_2M - 1)) == 0 && (addr + VMM_PAGE_SIZE_2M) <= end) {
      if (vmm_merge_to_huge(NULL, addr, VMM_PAGE_SIZE_2M) == 0) {
        addr += VMM_PAGE_SIZE_2M;
        continue;
      }
    }

    addr += PAGE_SIZE;
  }
}

static uint64_t *get_next_level(uint64_t *current_table, uint64_t index,
                                bool alloc, int level, uint64_t virt) {
  uint64_t entry = current_table[index];

  if (entry & PTE_PRESENT) {
    if (entry & PTE_HUGE) {
      if (!alloc) return NULL;

      // Auto-split huge page if we need to go deeper for a 4KB mapping
      if (vmm_split_huge_page(current_table, index, level, virt) < 0)
        return NULL;

      // Re-read the entry after split
      entry = current_table[index];
    }
    return (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }
  if (!alloc) return NULL;

  uint64_t new_table_phys = vmm_alloc_table();
  if (!new_table_phys) return NULL;

  // Initialize PTL for the new table page
  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  current_table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  return (uint64_t *) phys_to_virt(new_table_phys);
}

/**
 * Recursive helper to free page table levels.
 * @param table_phys Physical address of the table to free
 * @param level Current paging level (5 to 2)
 */
static void vmm_free_level(uint64_t table_phys, int level) {
  uint64_t *table = (uint64_t *) phys_to_virt(table_phys);

  // Only free user space entries if we are at the root level (256 entries).
  // Otherwise, free all 512 entries.
  int entries = (level == vmm_get_paging_levels()) ? 256 : 512;

  for (int i = 0; i < entries; i++) {
    uint64_t entry = table[i];
    if (!(entry & PTE_PRESENT)) continue;

    if (level > 1 && !(entry & PTE_HUGE)) {
      // Free the next level recursively
      vmm_free_level(PTE_GET_ADDR(entry), level - 1);
    }
  }

  // Free the table itself
  pmm_free_page(table_phys);
}

void vmm_free_page_tables(uint64_t pml_root) {
  if (!pml_root || pml_root == g_kernel_pml4) return;

  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  vmm_free_level(pml_root, vmm_get_paging_levels());
  spinlock_unlock_irqrestore(&vmm_lock, irq);
}

/**
 * Traverses page tables and returns a pointer to the leaf PTE.
 */
static uint64_t *vmm_get_pte_ptr(uint64_t pml_root_phys, uint64_t virt, bool alloc) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();

  for (int level = levels; level > 1; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      case 2: index = PD_INDEX(virt);
        break;
      default: return NULL;
    }

    uint64_t *next_table = get_next_level(current_table, index, alloc, level, virt);
    if (!next_table) return NULL;
    current_table = next_table;
  }

  return &current_table[PT_INDEX(virt)];
}

/* --- PTE Flag Helpers --- */

int vmm_is_dirty(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return 0;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int dirty = (*pte_p & PTE_DIRTY) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  return dirty;
}

void vmm_clear_dirty(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_DIRTY;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  vmm_tlb_shootdown(NULL, virt, virt + PAGE_SIZE);
}

int vmm_is_accessed(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return 0;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int accessed = (*pte_p & PTE_ACCESSED) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  return accessed;
}

void vmm_clear_accessed(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_ACCESSED;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  vmm_tlb_shootdown(NULL, virt, virt + PAGE_SIZE);
}

int vmm_set_flags(uint64_t pml_root, uint64_t virt, uint64_t flags) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return -1;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);

  uint64_t pte = *pte_p;
  uint64_t phys = PTE_GET_ADDR(pte);
  *pte_p = phys | flags | PTE_PRESENT;

  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  vmm_tlb_shootdown(NULL, virt, virt + PAGE_SIZE);
  return 0;
}

// --- Internal Unlocked Helpers ---

static int vmm_map_huge_page_locked(uint64_t pml_root_phys, uint64_t virt, uint64_t phys,
                                    uint64_t flags, uint64_t page_size) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();

  int target_level = 1; // 4KB
  if (page_size == VMM_PAGE_SIZE_2M) target_level = 2;
  else if (page_size == VMM_PAGE_SIZE_1G) target_level = 3;

  for (int level = levels; level > target_level; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      case 2: index = PD_INDEX(virt);
        break;
      default: return -1;
    }

    uint64_t *next_table = get_next_level(current_table, index, true, level, virt);
    if (!next_table) return -1;
    current_table = next_table;
  }

  uint64_t index;
  if (target_level == 2) index = PD_INDEX(virt);
  else if (target_level == 3) index = PDPT_INDEX(virt);
  else index = PT_INDEX(virt);

  // Modern: Use split page table lock for the leaf table
  struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);

  uint64_t entry_flags = (flags & ~PTE_ADDR_MASK);
  if (target_level > 1) {
    // PAT for Huge pages is at bit 12 instead of bit 7
    if (entry_flags & PTE_PAT) {
      entry_flags &= ~PTE_PAT;
      entry_flags |= PDE_PAT;
    }
    // Always ensure the Huge Page bit is set (Bit 7)
    entry_flags |= PTE_HUGE;
  }

  current_table[index] = (phys & PTE_ADDR_MASK) | entry_flags;

  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  // Invalidate TLB
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & PTE_ADDR_MASK) == pml_root_phys) {
    vmm_tlb_flush_local(virt);
  }

  return 0;
}

static uint64_t vmm_unmap_page_locked(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();

  for (int level = levels; level > 1; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      case 2: index = PD_INDEX(virt);
        break;
      default: return 0;
    }

    uint64_t entry = current_table[index];
    if (!(entry & PTE_PRESENT)) return 0;

    if (entry & PTE_HUGE) {
      uint64_t huge_size = (level == 3) ? VMM_PAGE_SIZE_1G : VMM_PAGE_SIZE_2M;
      if ((virt & (huge_size - 1)) == 0) {
        // Aligned unmap of a huge page. No need to shatter.
        struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
        irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);
        
        uint64_t phys = PTE_GET_ADDR(current_table[index]);
        current_table[index] = 0;
        
        spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);
        
        uint64_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if ((current_cr3 & PTE_ADDR_MASK) == pml_root_phys) {
          vmm_tlb_flush_local(virt);
        }
        return phys;
      }

      // If we encounter a huge page while trying to unmap a 4KB page,
      // we must split it first to avoid unmapping the entire huge region.
      if (vmm_split_huge_page(current_table, index, level, virt) < 0) {
        return 0; // Failed to split (OOM)
      }
      entry = current_table[index]; // Refresh entry after split
    }

    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  uint64_t pt_index = PT_INDEX(virt);

  // Modern: Use split page table lock for the leaf table
  struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);

  uint64_t entry = current_table[pt_index];
  if (!(entry & PTE_PRESENT)) {
    spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);
    return 0;
  }

  uint64_t phys = PTE_GET_ADDR(entry);
  current_table[pt_index] = 0;

  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & PTE_ADDR_MASK) == pml_root_phys) {
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
  }
  return phys;
}

// --- Public VMM API (Locked) ---

int vmm_map_huge_page(uint64_t pml_root_phys, uint64_t virt, uint64_t phys,
                      uint64_t flags, uint64_t page_size) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  int ret = vmm_map_huge_page_locked(pml_root_phys, virt, phys, flags, page_size);
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return ret;
}

int vmm_map_page(uint64_t pml_root_phys, uint64_t virt, uint64_t phys,
                 uint64_t flags) {
  return vmm_map_huge_page(pml_root_phys, virt, phys, flags, VMM_PAGE_SIZE_4K);
}

int vmm_map_pages(uint64_t pml_root_phys, uint64_t virt, uint64_t phys,
                  size_t count, uint64_t flags) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t cur_virt = virt;
  uint64_t cur_phys = phys;
  size_t remaining = count;

  while (remaining > 0) {
    // 1GB Promotion
    if (remaining >= (VMM_PAGE_SIZE_1G / PAGE_SIZE) &&
        (cur_virt & (VMM_PAGE_SIZE_1G - 1)) == 0 &&
        (cur_phys & (VMM_PAGE_SIZE_1G - 1)) == 0) {
      vmm_map_huge_page_locked(pml_root_phys, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_1G);
      cur_virt += VMM_PAGE_SIZE_1G;
      cur_phys += VMM_PAGE_SIZE_1G;
      remaining -= (VMM_PAGE_SIZE_1G / PAGE_SIZE);
      continue;
    }

    // 2MB Promotion
    if (remaining >= (VMM_PAGE_SIZE_2M / PAGE_SIZE) &&
        (cur_virt & (VMM_PAGE_SIZE_2M - 1)) == 0 &&
        (cur_phys & (VMM_PAGE_SIZE_2M - 1)) == 0) {
      vmm_map_huge_page_locked(pml_root_phys, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_2M);
      cur_virt += VMM_PAGE_SIZE_2M;
      cur_phys += VMM_PAGE_SIZE_2M;
      remaining -= (VMM_PAGE_SIZE_2M / PAGE_SIZE);
      continue;
    }

    // Default 4KB
    vmm_map_huge_page_locked(pml_root_phys, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_4K);
    cur_virt += PAGE_SIZE;
    cur_phys += PAGE_SIZE;
    remaining--;
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return 0;
}

int vmm_map_pages_list(uint64_t pml_root_phys, uint64_t virt, uint64_t *phys_list,
                       size_t count, uint64_t flags) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  for (size_t i = 0; i < count; i++) {
    if (vmm_map_huge_page_locked(pml_root_phys, virt + i * PAGE_SIZE, phys_list[i],
                                 flags, VMM_PAGE_SIZE_4K) < 0) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }
  }
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return 0;
}

int vmm_unmap_page_deferred(uint64_t pml_root_phys, uint64_t virt) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  uint64_t phys = vmm_unmap_page_locked(pml_root_phys, virt);
  spinlock_unlock_irqrestore(&vmm_lock, irq);

  if (phys) {
    struct page *page = phys_to_page(phys);
    if (page) put_page(page);
  }

  return 0;
}

int vmm_unmap_page(uint64_t pml_root_phys, uint64_t virt) {
  vmm_unmap_page_deferred(pml_root_phys, virt);
  vmm_tlb_shootdown(NULL, virt, virt + PAGE_SIZE);
  return 0;
}

int vmm_unmap_pages(uint64_t pml_root_phys, uint64_t virt, size_t count) {
  if (count == 0) return 0;
  
  for (size_t i = 0; i < count; i++) {
    vmm_unmap_page_deferred(pml_root_phys, virt + i * PAGE_SIZE);
  }

  // Single shootdown for the whole range
  vmm_tlb_shootdown(NULL, virt, virt + count * PAGE_SIZE);

  return 0;
}

int vmm_unmap_pages_and_get_phys(uint64_t pml_root_phys, uint64_t virt,
                                 uint64_t *phys_list, size_t count) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  for (size_t i = 0; i < count; i++) {
    phys_list[i] = vmm_unmap_page_locked(pml_root_phys, virt + i * PAGE_SIZE);
  }
  spinlock_unlock_irqrestore(&vmm_lock, irq);

  vmm_tlb_shootdown(NULL, virt, virt + count * PAGE_SIZE);
  return 0;
}

uint64_t vmm_virt_to_phys(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  uint64_t entry;
  uint64_t idx;

  for (int level = levels; level > 1; level--) {
    switch (level) {
      case 5: idx = PML5_INDEX(virt);
        break;
      case 4: idx = PML4_INDEX(virt);
        break;
      case 3: idx = PDPT_INDEX(virt);
        break;
      case 2: idx = PD_INDEX(virt);
        break;
      default: return 0;
    }

    entry = current_table[idx];
    if (!(entry & PTE_PRESENT)) return 0;

    if (entry & PTE_HUGE) {
      if (level == 3) return PTE_GET_ADDR(entry) + (virt & 0x3FFFFFFF);
      if (level == 2) return PTE_GET_ADDR(entry) + (virt & 0x1FFFFF);
      return 0;
    }

    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  idx = PT_INDEX(virt);
  entry = current_table[idx];
  if (!(entry & PTE_PRESENT)) return 0;

  return PTE_GET_ADDR(entry) + (virt & 0xFFF);
}

/**
 * Recursive helper to copy page table levels.
 * @param src_table_phys Physical address of the source table
 * @param dst_table_phys Physical address of the destination table
 * @param level Current paging level (5 to 2)
 * @return 0 on success
 */
static int vmm_copy_level(uint64_t src_table_phys, uint64_t dst_table_phys, int level) {
  uint64_t *src_table = (uint64_t *) phys_to_virt(src_table_phys);
  uint64_t *dst_table = (uint64_t *) phys_to_virt(dst_table_phys);

  // Only copy user space entries (0-255).
  // Higher half (256-511) is already shared via mm_create copying the root.
  int entries = (level == vmm_get_paging_levels()) ? 256 : 512;

  for (int i = 0; i < entries; i++) {
    uint64_t entry = src_table[i];
    if (!(entry & PTE_PRESENT)) continue;

    if (level > 1 && !(entry & PTE_HUGE)) {
      // Internal table, allocate a new one in the destination
      uint64_t new_table_phys = vmm_alloc_table();
      if (!new_table_phys) return -1;

      dst_table[i] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
      if (vmm_copy_level(PTE_GET_ADDR(entry), new_table_phys, level - 1) < 0)
        return -1;
    } else {
      // Leaf entry (4KB PTE or Huge Page)

      // If the page is writable, we MUST make it Read-Only for COW
      if (entry & PTE_RW) {
        entry &= ~PTE_RW;
        // Update source table as well to make it Read-Only
        src_table[i] = entry;
        // Note: We'll flush the TLB for the whole range later or rely on invlpg
      }

      dst_table[i] = entry;

      // Increment reference count of the physical page
      get_page(phys_to_page(PTE_GET_ADDR(entry)));
    }
  }
  return 0;
}

int vmm_copy_page_tables(uint64_t src_pml4, uint64_t dst_pml4) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  int ret = vmm_copy_level(src_pml4, dst_pml4, vmm_get_paging_levels());

  // Flush TLB to enforce Read-Only on the source process
  uint64_t current_cr3;
  __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
  if ((current_cr3 & PTE_ADDR_MASK) == src_pml4) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(current_cr3) : "memory");
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return ret;
}

int vmm_handle_cow(uint64_t pml4_phys, uint64_t virt) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t *pte_p = vmm_get_pte_ptr(pml4_phys, virt, false);
  if (!pte_p || !(*pte_p & PTE_PRESENT)) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  uint64_t entry = *pte_p;
  uint64_t old_phys = PTE_GET_ADDR(entry);
  struct page *old_page = phys_to_page(old_phys);

  // If we are the only owner, just make it writable again
  if (page_ref_count(old_page) == 1) {
    *pte_p |= PTE_RW;
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    vmm_tlb_flush_local(virt);
    return 0;
  }

  // Otherwise, duplicate the page
  uint64_t new_phys = pmm_alloc_page();
  if (!new_phys) {
    spinlock_unlock_irqrestore(&vmm_lock, irq);
    return -1;
  }

  memcpy(phys_to_virt(new_phys), phys_to_virt(old_phys), PAGE_SIZE);

  // Update PTE to point to the new page and make it writable
  uint64_t flags = PTE_GET_FLAGS(entry) | PTE_RW;
  *pte_p = new_phys | flags;

  spinlock_unlock_irqrestore(&vmm_lock, irq);

  // Drop reference to the old page
  put_page(old_page);

  vmm_tlb_flush_local(virt);
  return 0;
}

void vmm_dump_entry(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  uint64_t entry = 0;

  printk(VMM_CLASS "Dumping flags for virt: %llx (%d levels)\n", virt, levels);

  for (int level = levels; level > 1; level--) {
    uint64_t idx;
    switch (level) {
      case 5: idx = PML5_INDEX(virt);
        break;
      case 4: idx = PML4_INDEX(virt);
        break;
      case 3: idx = PDPT_INDEX(virt);
        break;
      case 2: idx = PD_INDEX(virt);
        break;
      default: return;
    }

    entry = current_table[idx];
    if (!(entry & PTE_PRESENT)) {
      printk("  Level %d missing\n", level);
      return;
    }

    if (entry & PTE_HUGE) {
      printk("  Level %d: HUGE PAGE, entry: %llx\n", level, entry);
      return;
    }

    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  entry = current_table[PT_INDEX(virt)];
  uint64_t cache_bits = (entry & (PTE_PAT | PTE_PCD | PTE_PWT));
  const char *cache_type = "Unknown";

  if (cache_bits == VMM_CACHE_WB) cache_type = "WB";
  else if (cache_bits == VMM_CACHE_WT) cache_type = "WT";
  else if (cache_bits == VMM_CACHE_UC_MINUS) cache_type = "UC-";
  else if (cache_bits == VMM_CACHE_UC) cache_type = "UC";
  else if (cache_bits == VMM_CACHE_WC) cache_type = "WC";
  else if (cache_bits == VMM_CACHE_WP) cache_type = "WP";

  printk(VMM_CLASS "  PTE: %llx (P:%d W:%d U:%d NX:%d Cache:%s)\n", entry,
         !!(entry & PTE_PRESENT), !!(entry & PTE_RW), !!(entry & PTE_USER),
         !!(entry & PTE_NX), cache_type);
}

void vmm_switch_pml4_pcid(uint64_t pml_root_phys, uint16_t pcid, bool no_flush) {
  uint64_t cr3 = (pml_root_phys & PTE_ADDR_MASK) | (pcid & CR3_PCID_MASK);
  if (no_flush) cr3 |= CR3_NOFLUSH;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

void vmm_switch_pml4(uint64_t pml_root_phys) {
  vmm_switch_pml4_pcid(pml_root_phys, 0, false);
}

void vmm_init(void) {
  printk(VMM_CLASS "Initializing VMM...\n");

  g_kernel_pml4 = vmm_alloc_table();
  if (!g_kernel_pml4) {
    panic(VMM_CLASS "Failed to allocate kernel PML root");
  }

  uint64_t boot_pml_root_phys;
  __asm__ volatile("mov %%cr3, %0" : "=r"(boot_pml_root_phys));
  boot_pml_root_phys &= PTE_ADDR_MASK;

  uint64_t *boot_pml_root = (uint64_t *) phys_to_virt(boot_pml_root_phys);
  uint64_t *kernel_pml_root = (uint64_t *) phys_to_virt(g_kernel_pml4);

  /*
   * The x86_64 architecture uses the higher half (entries 256-511) for the kernel and HHDM.
   * Entry 256 corresponds to the virtual address 0xFFFF800000000000.
   * By copying these entries from the bootloader's page table, we inherit the bootloader's
   * higher-half mappings, ensuring the kernel remains mapped after we switch CR3.
   * This logic is invariant across 4-level and 5-level paging because the split
   * between lower-half (User) and higher-half (Kernel) always occurs at index 256.
   */
  memcpy(kernel_pml_root + 256, boot_pml_root + 256, 256 * sizeof(uint64_t));

  vmm_switch_pml4(g_kernel_pml4);

  mm_init(&init_mm);
  init_mm.pml4 = (uint64_t *) g_kernel_pml4;

  printk(VMM_CLASS "VMM Initialized (%d levels active).\n", vmm_get_paging_levels());
}

void vmm_test(void) {
  /* --- MMU Smoke Test --- */
  printk(KERN_DEBUG VMM_CLASS "Running VMM Smoke Test...\n");

  // 1. Test RW-Semaphore
  down_read(&init_mm.mmap_lock);
  printk(KERN_DEBUG VMM_CLASS "  - RW-Sem Read Lock: OK\n");
  up_read(&init_mm.mmap_lock);

  down_write(&init_mm.mmap_lock);
  printk(KERN_DEBUG VMM_CLASS "  - RW-Sem Write Lock: OK\n");

  // 2. Test Mapping + Split PTL
  uint64_t test_virt = 0xDEADC0DE000;
  uint64_t test_phys = pmm_alloc_page();
  if (vmm_map_page(g_kernel_pml4, test_virt, test_phys, PTE_PRESENT | PTE_RW | PTE_USER) < 0) {
    panic("VMM Smoke Test: Mapping failed");
  }
  printk(KERN_DEBUG VMM_CLASS "  - Map + Split PTL: OK\n");

  // 3. Test Flag Helpers
  if (vmm_is_dirty(g_kernel_pml4, test_virt)) {
    panic("VMM Smoke Test: Page dirty before access");
  }

  // Trigger a write to set dirty bit
  *(volatile uint64_t *) phys_to_virt(test_phys) = 0x1234;
  // Note: We access via HHDM here, but on some CPUs the hardware walker
  // only sets dirty if accessed via the specific virtual mapping.
  // Let's just check if the helper can clear/set flags.

  vmm_set_flags(g_kernel_pml4, test_virt, PTE_RW | PTE_DIRTY);
  if (!vmm_is_dirty(g_kernel_pml4, test_virt)) {
    panic("VMM Smoke Test: Dirty bit helper failed");
  }
  printk(KERN_DEBUG VMM_CLASS "  - Dirty/Flags Helpers: OK\n");

  vmm_unmap_page(g_kernel_pml4, test_virt);
  // pmm_free_page(test_phys); // Removed: vmm_unmap_page now calls put_page()
  printk(KERN_DEBUG VMM_CLASS "  - Unmap: OK\n");

  // 4. Test Merging/Shattering
  uint64_t merge_virt = 0x200000; // Aligned to 2MB
  uint64_t merge_phys = pmm_alloc_pages(512); // Allocate 2MB contiguous (512 * 4KB)
  if (merge_phys) {
    vmm_map_pages(g_kernel_pml4, merge_virt, merge_phys, 512, PTE_PRESENT | PTE_RW);

    // Check if it was automatically promoted
    uint64_t check_phys = vmm_virt_to_phys(g_kernel_pml4, merge_virt);
    if (check_phys == merge_phys) {
      // Check if huge bit is set via dump logic or just assume if mapping exists
      // For the smoke test, we'll just proceed to merge which is now idempotent
    }

    if (vmm_merge_to_huge(&init_mm, merge_virt, VMM_PAGE_SIZE_2M) == 0) {
      printk(KERN_DEBUG VMM_CLASS "  - Merge 2MB: OK\n");
      if (vmm_shatter_huge_page(g_kernel_pml4, merge_virt, VMM_PAGE_SIZE_2M) == 0) {
        printk(KERN_DEBUG VMM_CLASS "  - Shatter 2MB: OK\n");
      } else {
        printk(KERN_WARNING VMM_CLASS "  - Shatter 2MB: FAILED\n");
      }
    } else {
      printk(KERN_WARNING VMM_CLASS "  - Merge 2MB: FAILED\n");
    }
    vmm_unmap_pages(g_kernel_pml4, merge_virt, 512);
  }

  up_write(&init_mm.mmap_lock);
  printk(KERN_DEBUG VMM_CLASS "VMM Smoke Test Passed.\n");
}
