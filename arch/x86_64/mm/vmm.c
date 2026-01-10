///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/mm/vmm.c
 * @brief Virtual Memory Manager for x86_64 (Split Page Table Locking)
 * @copyright (C) 2025 assembler-0
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/vma.h>
#include <arch/x86_64/mm/tlb.h>
#include <mm/page.h>
#include <mm/mmu_gather.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/features/features.h>
#include <aerosync/errno.h>
#include <linux/rcupdate.h>

// Global kernel PML root (physical address)
uint64_t g_kernel_pml_root = 0;

// Helper to access physical memory using the HHDM
static inline void *phys_to_virt(uint64_t phys) {
  return pmm_phys_to_virt(phys);
}

// Helper to allocate a zeroed page table frame
static uint64_t vmm_alloc_table_node(int nid) {
  struct folio *folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!folio) return 0;

  uint64_t phys = folio_to_phys(folio);
  memset(phys_to_virt(phys), 0, PAGE_SIZE);
  return phys;
}

static uint64_t vmm_alloc_table(void) {
  return vmm_alloc_table_node(-1);
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

int vmm_page_size_supported(size_t size) {
  if (size == VMM_PAGE_SIZE_4K) return 1;
  if (size == VMM_PAGE_SIZE_2M) return 1;
  if (size == VMM_PAGE_SIZE_1G) return get_cpu_features()->pdpe1gb;
  return 0;
}

static int vmm_split_huge_page(uint64_t *table, uint64_t index, int level, uint64_t virt, int nid) {
  uint64_t entry = table[index];
  uint64_t new_table_phys = vmm_alloc_table_node(nid);
  if (!new_table_phys) return -ENOMEM;

  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  uint64_t *new_table = (uint64_t *) phys_to_virt(new_table_phys);
  uint64_t base_phys = PTE_GET_ADDR(entry);
  uint64_t flags = PTE_GET_FLAGS(entry) & ~PTE_HUGE;

  uint64_t step;
  uint64_t child_flags = flags;

  if (level == 3) {
    step = VMM_PAGE_SIZE_2M;
    child_flags |= PTE_HUGE;
  } else {
    step = VMM_PAGE_SIZE_4K;
    if (flags & PDE_PAT) {
      child_flags &= ~PDE_PAT;
      child_flags |= PTE_PAT;
    }
  }

  for (int i = 0; i < 512; i++) {
    new_table[i] = (base_phys + (i * step)) | child_flags | PTE_PRESENT;
  }

  table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
  return 0;
}

/* Internal helper: Lock a specific level table and return ptr */
static uint64_t *vmm_lock_table(uint64_t *table, irq_flags_t *flags) {
  struct page *pg = phys_to_page(pmm_virt_to_phys(table));
  *flags = spinlock_lock_irqsave(&pg->ptl);
  return table;
}

static void vmm_unlock_table(uint64_t *table, irq_flags_t flags) {
  struct page *pg = phys_to_page(pmm_virt_to_phys(table));
  spinlock_unlock_irqrestore(&pg->ptl, flags);
}

static uint64_t *get_next_level(uint64_t *current_table, uint64_t index, bool alloc, int level, uint64_t virt, int nid,
                                int *out_level) {
  irq_flags_t flags;
  vmm_lock_table(current_table, &flags);

  uint64_t entry = current_table[index];

  if (entry & PTE_PRESENT) {
    if (entry & PTE_HUGE) {
      if (!alloc) {
        if (out_level) *out_level = level;
        uint64_t *result = &current_table[index];
        vmm_unlock_table(current_table, flags);
        return result;
      }
      if (vmm_split_huge_page(current_table, index, level, virt, nid) < 0) {
        vmm_unlock_table(current_table, flags);
        return NULL;
      }
      entry = current_table[index];
    }
    uint64_t *result = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
    vmm_unlock_table(current_table, flags);
    return result;
  }

  if (!alloc) {
    vmm_unlock_table(current_table, flags);
    return NULL;
  }

  uint64_t new_table_phys = vmm_alloc_table_node(nid);
  if (!new_table_phys) {
    vmm_unlock_table(current_table, flags);
    return NULL;
  }

  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  uint64_t new_entry = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  current_table[index] = new_entry;

  vmm_unlock_table(current_table, flags);
  return (uint64_t *) phys_to_virt(new_table_phys);
}

static uint64_t *vmm_get_pte_ptr(uint64_t pml_root_phys, uint64_t virt, bool alloc, int nid, int *out_level) {
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

    int next_out_level = 0;
    uint64_t *next_table = get_next_level(current_table, index, alloc, level, virt, nid, &next_out_level);
    if (!next_table) return NULL;

    if (!alloc && next_out_level != 0) {
      if (out_level) *out_level = next_out_level;
      return next_table;
    }
    current_table = next_table;
  }
  if (out_level) *out_level = 1;
  return &current_table[PT_INDEX(virt)];
}

int vmm_merge_to_huge(struct mm_struct *mm, uint64_t virt, uint64_t target_huge_size) {
  if (!mm) mm = &init_mm;
  if (target_huge_size != VMM_PAGE_SIZE_2M && target_huge_size != VMM_PAGE_SIZE_1G)
    return -EINVAL;
  if (virt & (target_huge_size - 1)) return -EINVAL;

  int levels = vmm_get_paging_levels();
  int target_level = (target_huge_size == VMM_PAGE_SIZE_2M) ? 2 : 3;
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);

  for (int level = levels; level > target_level; level--) {
    uint64_t index;
    switch (level) {
      case 5: index = PML5_INDEX(virt);
        break;
      case 4: index = PML4_INDEX(virt);
        break;
      case 3: index = PDPT_INDEX(virt);
        break;
      default: return -EINVAL;
    }
    uint64_t entry = current_table[index];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return -ENOENT;
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  uint64_t idx = (target_level == 2) ? PD_INDEX(virt) : PDPT_INDEX(virt);
  irq_flags_t ptl_flags;
  vmm_lock_table(current_table, &ptl_flags);

  uint64_t entry = current_table[idx];
  if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) {
    vmm_unlock_table(current_table, ptl_flags);
    return (entry & PTE_HUGE) ? 0 : -ENOENT;
  }

  uint64_t sub_table_phys = PTE_GET_ADDR(entry);
  uint64_t *sub_table = (uint64_t *) phys_to_virt(sub_table_phys);

  /* Validate entries under sub-table lock too? Actually bridge promotion usually requires quiescing other users */
  uint64_t first_entry = sub_table[0];
  uint64_t base_phys = PTE_GET_ADDR(first_entry);
  uint64_t flags = PTE_GET_FLAGS(first_entry);
  uint64_t step = (target_level == 2) ? VMM_PAGE_SIZE_4K : VMM_PAGE_SIZE_2M;

  for (int i = 0; i < 512; i++) {
    uint64_t e = sub_table[i];
    if (!(e & PTE_PRESENT) || PTE_GET_ADDR(e) != (base_phys + (i * step)) || PTE_GET_FLAGS(e) != flags) {
      vmm_unlock_table(current_table, ptl_flags);
      return -EADDRNOTAVAIL;
    }
  }

  if (base_phys & (target_huge_size - 1)) {
    vmm_unlock_table(current_table, ptl_flags);
    return -EINVAL;
  }

  uint64_t huge_flags = flags | PTE_HUGE;
  if (target_level == 2 && (huge_flags & PTE_PAT)) {
    huge_flags &= ~PTE_PAT;
    huge_flags |= PDE_PAT;
  }

  current_table[idx] = base_phys | huge_flags;
  vmm_unlock_table(current_table, ptl_flags);

  vmm_tlb_shootdown(mm, virt, virt + target_huge_size);
  pmm_free_page(sub_table_phys);
  return 0;
}

int vmm_shatter_huge_page(struct mm_struct *mm, uint64_t virt, uint64_t large_page_size) {
  if (!mm) mm = &init_mm;
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
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
      default: return -EINVAL;
    }
    uint64_t entry = current_table[index];
    if (!(entry & PTE_PRESENT) || (entry & PTE_HUGE)) return -ENOENT;
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  uint64_t idx = (target_level == 2) ? PD_INDEX(virt) : PDPT_INDEX(virt);
  irq_flags_t ptl_flags;
  vmm_lock_table(current_table, &ptl_flags);

  if (!(current_table[idx] & PTE_PRESENT) || !(current_table[idx] & PTE_HUGE)) {
    vmm_unlock_table(current_table, ptl_flags);
    return -EINVAL;
  }

  int ret = vmm_split_huge_page(current_table, idx, target_level, virt, mm->preferred_node);
  vmm_unlock_table(current_table, ptl_flags);
  return ret;
}

void vmm_merge_range(struct mm_struct *mm, uint64_t start, uint64_t end) {
  uint64_t addr = PAGE_ALIGN_UP(start);
  while (addr < end) {
    if ((addr & (VMM_PAGE_SIZE_1G - 1)) == 0 && (addr + VMM_PAGE_SIZE_1G) <= end) {
      if (vmm_merge_to_huge(mm, addr, VMM_PAGE_SIZE_1G) == 0) {
        addr += VMM_PAGE_SIZE_1G;
        continue;
      }
    }
    if ((addr & (VMM_PAGE_SIZE_2M - 1)) == 0 && (addr + VMM_PAGE_SIZE_2M) <= end) {
      if (vmm_merge_to_huge(mm, addr, VMM_PAGE_SIZE_2M) == 0) {
        addr += VMM_PAGE_SIZE_2M;
        continue;
      }
    }
    addr += PAGE_SIZE;
  }
}

static void vmm_free_level(uint64_t table_phys, int level) {
  uint64_t *table = (uint64_t *) phys_to_virt(table_phys);
  int entries = (level == vmm_get_paging_levels()) ? 256 : 512;
  for (int i = 0; i < entries; i++) {
    uint64_t entry = table[i];
    if (!(entry & PTE_PRESENT)) continue;
    if (level > 1 && !(entry & PTE_HUGE)) {
      vmm_free_level(PTE_GET_ADDR(entry), level - 1);
    } else {
      /* Leaf entry (4KB or Huge): release the reference to the data page */
      put_page(phys_to_page(PTE_GET_ADDR(entry)));
    }
  }
  pmm_free_page(table_phys);
}

void vmm_free_page_tables(struct mm_struct *mm) {
  if (!mm || (uint64_t) mm->pml_root == g_kernel_pml_root) return;
  /* At this point the mm is being destroyed, so no concurrent access should happen */
  vmm_free_level((uint64_t) mm->pml_root, vmm_get_paging_levels());
}

int vmm_is_dirty(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return 0;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int dirty = (*pte_p & PTE_DIRTY) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  return dirty;
}

void vmm_clear_dirty(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  int level = 0;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, &level);
  if (!pte_p) return;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_DIRTY;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  uint64_t size = (level == 2) ? VMM_PAGE_SIZE_2M : ((level == 3) ? VMM_PAGE_SIZE_1G : PAGE_SIZE);
  vmm_tlb_shootdown(mm, virt & ~(size - 1), (virt & ~(size - 1)) + size);
}

int vmm_is_accessed(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return 0;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int accessed = (*pte_p & PTE_ACCESSED) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  return accessed;
}

void vmm_clear_accessed(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  int level = 0;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, &level);
  if (!pte_p) return;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_ACCESSED;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  uint64_t size = (level == 2) ? VMM_PAGE_SIZE_2M : ((level == 3) ? VMM_PAGE_SIZE_1G : PAGE_SIZE);
  vmm_tlb_shootdown(mm, virt & ~(size - 1), (virt & ~(size - 1)) + size);
}

int vmm_set_flags(struct mm_struct *mm, uint64_t virt, uint64_t flags) {
  if (!mm) mm = &init_mm;
  int level = 0;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, &level);
  if (!pte_p) return -ENOENT;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);
  uint64_t entry_flags = flags;
  if (level > 1) {
    if (entry_flags & PTE_PAT) {
      entry_flags &= ~PTE_PAT;
      entry_flags |= PDE_PAT;
    }
    entry_flags |= PTE_HUGE;
  } else {
    if (entry_flags & PDE_PAT) {
      entry_flags &= ~PDE_PAT;
      entry_flags |= PTE_PAT;
    }
  }
  *pte_p = PTE_GET_ADDR(*pte_p) | entry_flags | PTE_PRESENT;
  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);
  uint64_t size = (level == 2) ? VMM_PAGE_SIZE_2M : ((level == 3) ? VMM_PAGE_SIZE_1G : PAGE_SIZE);
  vmm_tlb_shootdown(mm, virt & ~(size - 1), (virt & ~(size - 1)) + size);
  return 0;
}

static int vmm_map_huge_page_locked(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags,
                                    uint64_t page_size, int nid) {
  if (!vmm_page_size_supported(page_size)) return -EOPNOTSUPP;
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
  int levels = vmm_get_paging_levels();
  int target_level = (page_size == VMM_PAGE_SIZE_1G) ? 3 : (page_size == VMM_PAGE_SIZE_2M ? 2 : 1);
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
      default: return -EINVAL;
    }
    uint64_t *next_table = get_next_level(current_table, index, true, level, virt, nid, NULL);
    if (!next_table) return -ENOMEM;
    current_table = next_table;
  }
  uint64_t index = (target_level == 3) ? PDPT_INDEX(virt) : ((target_level == 2) ? PD_INDEX(virt) : PT_INDEX(virt));
  struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);
  uint64_t entry_flags = (flags & ~PTE_ADDR_MASK);
  if (target_level > 1) {
    if (entry_flags & PTE_PAT) {
      entry_flags &= ~PTE_PAT;
      entry_flags |= PDE_PAT;
    }
    entry_flags |= PTE_HUGE;
  }
  current_table[index] = (phys & PTE_ADDR_MASK) | entry_flags;
  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  vmm_tlb_shootdown(mm, virt & ~(page_size - 1), (virt & ~(page_size - 1)) + page_size);
  return 0;
}

static uint64_t vmm_unmap_page_locked(struct mm_struct *mm, uint64_t virt, int nid) {
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
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
    uint64_t entry = __atomic_load_n(&current_table[index], __ATOMIC_ACQUIRE);
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_HUGE) {
      uint64_t huge_size = (level == 3) ? VMM_PAGE_SIZE_1G : VMM_PAGE_SIZE_2M;
      if ((virt & (huge_size - 1)) == 0) {
        struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
        irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);
        uint64_t phys = PTE_GET_ADDR(current_table[index]);
        __atomic_store_n(&current_table[index], 0, __ATOMIC_RELEASE);
        spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

        vmm_tlb_shootdown(mm, virt, virt + huge_size);
        return phys;
      }
      if (vmm_split_huge_page(current_table, index, level, virt, nid) < 0) return 0;
      entry = __atomic_load_n(&current_table[index], __ATOMIC_ACQUIRE);
    }
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }
  struct page *table_page = phys_to_page(pmm_virt_to_phys(current_table));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);
  uint64_t phys = PTE_GET_ADDR(current_table[PT_INDEX(virt)]);
  __atomic_store_n(&current_table[PT_INDEX(virt)], 0, __ATOMIC_RELEASE);
  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  vmm_tlb_shootdown(mm, virt, virt + PAGE_SIZE);
  return phys;
}

int vmm_map_huge_page(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t page_size) {
  if (!mm) mm = &init_mm;
  return vmm_map_huge_page_locked(mm, virt, phys, flags, page_size, mm->preferred_node);
}

int vmm_map_page(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags) {
  return vmm_map_huge_page(mm, virt, phys, flags, VMM_PAGE_SIZE_4K);
}

int vmm_map_pages(struct mm_struct *mm, uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
  if (!mm) mm = &init_mm;
  uint64_t cur_virt = virt, cur_phys = phys;
  size_t remaining = count;
  while (remaining > 0) {
    if (remaining >= 262144 && (cur_virt & (VMM_PAGE_SIZE_1G - 1)) == 0 && (cur_phys & (VMM_PAGE_SIZE_1G - 1)) == 0 &&
        vmm_page_size_supported(VMM_PAGE_SIZE_1G)) {
      vmm_map_huge_page_locked(mm, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_1G, mm->preferred_node);
      cur_virt += VMM_PAGE_SIZE_1G;
      cur_phys += VMM_PAGE_SIZE_1G;
      remaining -= 262144;
      continue;
    }
    if (remaining >= 512 && (cur_virt & (VMM_PAGE_SIZE_2M - 1)) == 0 && (cur_phys & (VMM_PAGE_SIZE_2M - 1)) == 0 &&
        vmm_page_size_supported(VMM_PAGE_SIZE_2M)) {
      vmm_map_huge_page_locked(mm, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_2M, mm->preferred_node);
      cur_virt += VMM_PAGE_SIZE_2M;
      cur_phys += VMM_PAGE_SIZE_2M;
      remaining -= 512;
      continue;
    }
    vmm_map_huge_page_locked(mm, cur_virt, cur_phys, flags, VMM_PAGE_SIZE_4K, mm->preferred_node);
    cur_virt += PAGE_SIZE;
    cur_phys += PAGE_SIZE;
    remaining--;
  }
  return 0;
}

int vmm_map_pages_list(struct mm_struct *mm, uint64_t virt, const uint64_t *phys_list, size_t count, uint64_t flags) {
  if (!mm) mm = &init_mm;
  for (size_t i = 0; i < count; i++)
    vmm_map_huge_page_locked(mm, virt + i * PAGE_SIZE, phys_list[i], flags,
                             VMM_PAGE_SIZE_4K, mm->preferred_node);
  return 0;
}

uint64_t vmm_unmap_page_no_flush(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  return vmm_unmap_page_locked(mm, virt, mm->preferred_node);
}

struct folio *vmm_unmap_folio_no_flush(struct mm_struct *mm, uint64_t virt) {
  uint64_t phys = vmm_unmap_page_no_flush(mm, virt);
  if (!phys) return NULL;
  return page_folio(phys_to_page(phys));
}

int vmm_unmap_page(struct mm_struct *mm, uint64_t virt) {
  struct folio *folio = vmm_unmap_folio_no_flush(mm, virt);
  vmm_tlb_shootdown(mm, virt, virt + PAGE_SIZE);
  if (folio) {
    folio_put(folio);
  }
  return 0;
}

struct folio *vmm_unmap_folio(struct mm_struct *mm, uint64_t virt) {
  struct folio *folio = vmm_unmap_folio_no_flush(mm, virt);
  vmm_tlb_shootdown(mm, virt, virt + PAGE_SIZE);
  return folio;
}

int vmm_unmap_pages(struct mm_struct *mm, uint64_t virt, size_t count) {
  if (count == 0) return 0;
  struct mmu_gather tlb;
  tlb_gather_mmu(&tlb, mm, virt, virt + count * PAGE_SIZE);
  for (size_t i = 0; i < count; i++) {
    struct folio *folio = vmm_unmap_folio_no_flush(mm, virt + i * PAGE_SIZE);
    if (folio) tlb_remove_folio(&tlb, folio, virt + i * PAGE_SIZE);
  }
  tlb_finish_mmu(&tlb);
  return 0;
}

int vmm_unmap_pages_and_get_folios(struct mm_struct *mm, uint64_t virt, struct folio **folios, size_t count) {
  if (!mm) mm = &init_mm;
  for (size_t i = 0; i < count; i++) {
    folios[i] = vmm_unmap_folio_no_flush(mm, virt + i * PAGE_SIZE);
  }
  vmm_tlb_shootdown(mm, virt, virt + count * PAGE_SIZE);
  return 0;
}

uint64_t vmm_virt_to_phys(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
  int levels = vmm_get_paging_levels();
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
      default: return 0;
    }
    uint64_t entry = current_table[idx];
    if (!(entry & PTE_PRESENT)) return 0;
    if (entry & PTE_HUGE) {
      if (level == 3) return PTE_GET_ADDR(entry) + (virt & 0x3FFFFFFF);
      if (level == 2) return PTE_GET_ADDR(entry) + (virt & 0x1FFFFF);
      return 0;
    }
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }
  uint64_t entry = current_table[PT_INDEX(virt)];
  if (!(entry & PTE_PRESENT)) return 0;
  return PTE_GET_ADDR(entry) + (virt & 0xFFF);
}

static int vmm_copy_level(uint64_t src_table_phys, uint64_t dst_table_phys, int level) {
  uint64_t *src_table = (uint64_t *) phys_to_virt(src_table_phys);
  uint64_t *dst_table = (uint64_t *) phys_to_virt(dst_table_phys);
  int entries = (level == vmm_get_paging_levels()) ? 256 : 512;
  for (int i = 0; i < entries; i++) {
    uint64_t entry = src_table[i];
    if (!(entry & PTE_PRESENT)) continue;
    if (level > 1 && !(entry & PTE_HUGE)) {
      uint64_t new_table_phys = vmm_alloc_table();
      if (!new_table_phys) return -ENOMEM;
      dst_table[i] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
      int res = vmm_copy_level(PTE_GET_ADDR(entry), new_table_phys, level - 1);
      if (res < 0) return res;
    } else {
      /* Leaf PTE: Copy and handle COW */
      struct page *src_page = phys_to_page(src_table_phys);
      irq_flags_t flags = spinlock_lock_irqsave(&src_page->ptl);

      entry = src_table[i];
      if (entry & PTE_PRESENT) {
        if (entry & PTE_RW) {
          entry &= ~PTE_RW;
          src_table[i] = entry;
        }
        dst_table[i] = entry;
        get_page(phys_to_page(PTE_GET_ADDR(entry)));
      }

      spinlock_unlock_irqrestore(&src_page->ptl, flags);
    }
  }
  return 0;
}

int vmm_copy_page_tables(struct mm_struct *src_mm, const struct mm_struct *dst_mm) {
  /* Copying usually happens in fork, we need to quiesce src_mm or assume lock is held by caller */
  return vmm_copy_level((uint64_t) src_mm->pml_root, (uint64_t) dst_mm->pml_root, vmm_get_paging_levels());
}

int vmm_handle_cow(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr((uint64_t) mm->pml_root, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return -ENOENT;
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *) ((uint64_t) pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  uint64_t entry = *pte_p;
  if (!(entry & PTE_PRESENT)) {
    spinlock_unlock_irqrestore(&table_page->ptl, flags);
    return -ENOENT;
  }
  struct page *old_page = phys_to_page(PTE_GET_ADDR(entry));
  if (atomic_read(&old_page->_refcount) == 1) {
    *pte_p |= PTE_RW;
    spinlock_unlock_irqrestore(&table_page->ptl, flags);
    vmm_tlb_flush_local(virt);
    return 0;
  }
  get_page(old_page);
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  uint64_t new_phys = pmm_alloc_page();
  if (!new_phys) {
    put_page(old_page);
    return -ENOMEM;
  }

  memcpy(phys_to_virt(new_phys), phys_to_virt(PTE_GET_ADDR(entry)), PAGE_SIZE);

  flags = spinlock_lock_irqsave(&table_page->ptl);
  /* Re-check entry hasn't changed during allocation */
  if (*pte_p != entry) {
    spinlock_unlock_irqrestore(&table_page->ptl, flags);
    pmm_free_page(new_phys);
    put_page(old_page);
    /* Retry if the mapping still exists */
    if (vmm_virt_to_phys(mm, virt) == PTE_GET_ADDR(entry))
      return vmm_handle_cow(mm, virt);
    return 0;
  }

  *pte_p = new_phys | (PTE_GET_FLAGS(entry) | PTE_RW);
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  /* 
   * Release both references: 
   * 1. Our temporary one from get_page(old_page).
   * 2. The original reference from the page table mapping we just replaced.
   */
  put_page(old_page);
  put_page(old_page);

  /* Global TLB shootdown is mandatory for shared address spaces in SMP */
  vmm_tlb_shootdown(mm, virt, virt + PAGE_SIZE);
  return 0;
}

void vmm_dump_entry(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *) phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
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
    uint64_t entry = current_table[idx];
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
  uint64_t entry = current_table[PT_INDEX(virt)];
  uint64_t cache_bits = (entry & (PTE_PAT | PTE_PCD | PTE_PWT));
  const char *cache_type = "Unknown";
  if (cache_bits == VMM_CACHE_WB) cache_type = "WB";
  else if (cache_bits == VMM_CACHE_WT) cache_type = "WT";
  else if (cache_bits == VMM_CACHE_UC_MINUS) cache_type = "UC-";
  else if (cache_bits == VMM_CACHE_UC) cache_type = "UC";
  else if (cache_bits == VMM_CACHE_WC) cache_type = "WC";
  else if (cache_bits == VMM_CACHE_WP) cache_type = "WP";
  printk(VMM_CLASS "  PTE: %llx (P:%d W:%d U:%d NX:%d Cache:%s)\n", entry, !!(entry & PTE_PRESENT), !!(entry & PTE_RW),
         !!(entry & PTE_USER), !!(entry & PTE_NX), cache_type);
}

void vmm_switch_pml_root_pcid(uint64_t pml_root_phys, uint16_t pcid, bool no_flush) {
  uint64_t cr3 = (pml_root_phys & PTE_ADDR_MASK) | (pcid & CR3_PCID_MASK);
  if (no_flush) cr3 |= CR3_NOFLUSH;
  __asm__ volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
}

void vmm_switch_pml_root(uint64_t pml_root_phys) { vmm_switch_pml_root_pcid(pml_root_phys, 0, false); }

void vmm_init(void) {
  printk(VMM_CLASS "Initializing VMM...\n");
  cpu_features_t *features = get_cpu_features();
  if (!features) panic(VMM_CLASS "Failed to get CPU features");
  if (!features->nx) printk(KERN_WARNING VMM_CLASS "NX bit not supported - security reduced\n");
  if (!features->pdpe1gb) printk(KERN_INFO VMM_CLASS "1GB pages not supported\n");
  g_kernel_pml_root = vmm_alloc_table();
  if (!g_kernel_pml_root) panic(VMM_CLASS "Failed to allocate kernel PML root");
  uint64_t boot_pml_root_phys;
  __asm__ volatile("mov %%cr3, %0" : "=r"(boot_pml_root_phys));
  boot_pml_root_phys &= PTE_ADDR_MASK;
  uint64_t *boot_pml_root = (uint64_t *) phys_to_virt(boot_pml_root_phys);
  uint64_t *kernel_pml_root = (uint64_t *) phys_to_virt(g_kernel_pml_root);
  int levels = vmm_get_paging_levels();
  memcpy(kernel_pml_root + 256, boot_pml_root + 256, 256 * sizeof(uint64_t));
  vmm_switch_pml_root(g_kernel_pml_root);
  mm_init(&init_mm);
  init_mm.pml_root = (uint64_t *) g_kernel_pml_root;
  printk(VMM_CLASS "VMM Initialized (%d levels active, NX:%s, 1GB:%s).\n",
         levels, features->nx ? "yes" : "no", features->pdpe1gb ? "yes" : "no");
}

void vmm_test(void) {
  printk(KERN_DEBUG VMM_CLASS "Running VMM Production Stress Test...\n");
  
  /* Test 1: Basic Map/Unmap (Existing logic, cleaned up) */
  uint64_t test_virt = 0xDEADC0DE000;
  uint64_t test_phys = pmm_alloc_page();
  if (vmm_map_page(&init_mm, test_virt, test_phys, PTE_PRESENT | PTE_RW | PTE_USER) < 0) 
      panic("VMM Stress: Basic mapping failed");
  vmm_unmap_page(&init_mm, test_virt);
  printk(KERN_DEBUG VMM_CLASS "  - Basic Map/Unmap: OK\n");

  /* Test 2: COW Integrity Stress */
  printk(KERN_DEBUG VMM_CLASS "  - COW Integrity Stress: start...\n");
  struct mm_struct *parent_mm = mm_create();
  uint64_t cow_virt = 0x1000000;
  uint64_t cow_phys = pmm_alloc_page();
  memset(phys_to_virt(cow_phys), 0xAA, PAGE_SIZE);
  
  vmm_map_page(parent_mm, cow_virt, cow_phys, PTE_PRESENT | PTE_RW | PTE_USER);
  
  struct mm_struct *child_mm = mm_create();
  if (vmm_copy_page_tables(parent_mm, child_mm) < 0) panic("VMM Stress: vmm_copy_page_tables failed");

  /* Verify initial state: both see 0xAA */
  if (*(uint8_t *)phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt)) != 0xAA) panic("VMM Stress: Parent data mismatch");
  if (*(uint8_t *)phys_to_virt(vmm_virt_to_phys(child_mm, cow_virt)) != 0xAA) panic("VMM Stress: Child data mismatch");

  /* Trigger COW in parent */
  vmm_handle_cow(parent_mm, cow_virt);
  uint8_t *parent_ptr = (uint8_t *)phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt));
  *parent_ptr = 0xBB;

  /* Verify Isolation */
  if (*(uint8_t *)phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt)) != 0xBB) panic("VMM Stress: Parent COW failed");
  if (*(uint8_t *)phys_to_virt(vmm_virt_to_phys(child_mm, cow_virt)) != 0xAA) panic("VMM Stress: Child corrupted by parent!");

  mm_destroy(child_mm);
  mm_destroy(parent_mm);
  printk(KERN_DEBUG VMM_CLASS "  - COW Integrity Stress: OK\n");

  /* Test 3: Huge Page Shattering Stress */
  printk(KERN_DEBUG VMM_CLASS "  - Huge Page Shatter Stress: start...\n");
  struct mm_struct *hp_mm = mm_create();
  uint64_t hp_virt = 0x2000000;
  uint64_t hp_phys = pmm_alloc_pages(512); // 2MB
  memset(phys_to_virt(hp_phys), 0xCC, VMM_PAGE_SIZE_2M);

  vmm_map_huge_page(hp_mm, hp_virt, hp_phys, PTE_PRESENT | PTE_RW | PTE_USER, VMM_PAGE_SIZE_2M);
  
  /* Shatter the huge page by unmapping a single 4KB page in the middle */
  vmm_unmap_page(hp_mm, hp_virt + 128 * PAGE_SIZE);

  /* Verify that other pages are still present and have correct data */
  uint64_t check_phys = vmm_virt_to_phys(hp_mm, hp_virt);
  if (check_phys == 0 || *(uint8_t *)phys_to_virt(check_phys) != 0xCC) panic("VMM Stress: Shatter corrupted data");
  
  if (vmm_virt_to_phys(hp_mm, hp_virt + 128 * PAGE_SIZE) != 0) panic("VMM Stress: Unmap in huge page failed");

  mm_destroy(hp_mm);
  printk(KERN_DEBUG VMM_CLASS "  - Huge Page Shatter Stress: OK\n");

  printk(KERN_DEBUG VMM_CLASS "VMM Production Stress Test Passed.\n");
}
