///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file arch/x86_64/mm/vmm.c
 * @brief Virtual Memory Manager for x86_64 (Split Page Table Locking)
 * @copyright (C) 2025-2026 assembler-0
 */

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/mm/layout.h>
#include <aerosync/classes.h>
#include <aerosync/panic.h>
#include <aerosync/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/vma.h>
#include <mm/zone.h>
#include <arch/x86_64/mm/tlb.h>
#include <mm/page.h>
#include <mm/mmu_gather.h>
#include <arch/x86_64/smp.h>
#include <arch/x86_64/features/features.h>
#include <aerosync/errno.h>
#include <lib/math.h>

// Global kernel PML root (physical address)
uint64_t g_kernel_pml_root = 0;

// Helper to access physical memory using the HHDM
static inline void *phys_to_virt(uint64_t phys) {
  return pmm_phys_to_virt(phys);
}

/*
 * Per-CPU Pre-Zeroed Page Table Cache
 *
 * OPTIMIZATION: Page table allocation is a hot path during fork/mmap.
 * The memset() to zero a 4KB page is expensive. We maintain a small
 * per-CPU cache of pre-zeroed pages that can be used immediately.
 *
 * Pages are zeroed in the background or when the cache is refilled.
 */
#define PGT_CACHE_SIZE 4

struct pgt_cache {
  uint64_t pages[PGT_CACHE_SIZE];
  int count;
};

static DEFINE_PER_CPU(struct pgt_cache, pgt_cache);

static uint64_t pgt_cache_alloc(int nid) {
  preempt_disable();
  struct pgt_cache *cache = this_cpu_ptr(pgt_cache);

  if (cache->count > 0) {
    uint64_t phys = cache->pages[--cache->count];
    preempt_enable();
    return phys;
  }
  preempt_enable();

  /* Cache empty - allocate and zero synchronously */
  struct folio *folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!folio) return 0;

  uint64_t phys = folio_to_phys(folio);
  memset(phys_to_virt(phys), 0, PAGE_SIZE);
  return phys;
}

/* Refill the cache with pre-zeroed pages (call from idle or background) */
void pgt_cache_refill(void) {
  preempt_disable();
  struct pgt_cache *cache = this_cpu_ptr(pgt_cache);

  while (cache->count < PGT_CACHE_SIZE) {
    struct folio *folio = alloc_pages_node(-1, GFP_KERNEL | __GFP_NOWARN, 0);
    if (!folio) break;

    uint64_t phys = folio_to_phys(folio);
    memset(phys_to_virt(phys), 0, PAGE_SIZE);
    cache->pages[cache->count++] = phys;
  }
  preempt_enable();
}

// Helper to allocate a zeroed page table frame
uint64_t vmm_alloc_table_node(int nid) {
  return pgt_cache_alloc(nid);
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

static bool g_support_1gb = false;

int vmm_page_size_supported(size_t size) {
  if (size == VMM_PAGE_SIZE_4K) return 1;
  if (size == VMM_PAGE_SIZE_2M) return 1;
  if (size == VMM_PAGE_SIZE_1G) return g_support_1gb;
  return 0;
}

static int vmm_split_huge_page(struct mm_struct *mm, uint64_t *table, uint64_t index, int level, uint64_t virt,
                               int nid) {
  uint64_t entry = table[index];
  uint64_t new_table_phys = vmm_alloc_table_node(nid);
  if (!new_table_phys) return -ENOMEM;

  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  uint64_t *new_table = (uint64_t *) phys_to_virt(new_table_phys);
  uint64_t base_phys = (level == 3) ? (entry & 0x000FFFFFC0000000ULL) : (entry & 0x000FFFFFFFE00000ULL);
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

  /* 
   * Increment refcount for the new mappings.
   * We are replacing 1 huge mapping with 512 smaller mappings.
   */
  struct page *base_pg = phys_to_page(base_phys);
  if (base_pg) {
    folio_ref_add(page_folio(base_pg), 511);
  }

  for (int i = 0; i < 512; i++) {
    new_table[i] = (base_phys + (i * step)) | child_flags | PTE_PRESENT;
  }


  table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;

  uint64_t size = (level == 3) ? VMM_PAGE_SIZE_1G : VMM_PAGE_SIZE_2M;
  vmm_tlb_shootdown(mm, virt & ~(size - 1), (virt & ~(size - 1)) + size);
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

static uint64_t *get_next_level(struct mm_struct *mm, uint64_t *current_table, uint64_t index, bool alloc, int level,
                                uint64_t virt, int nid,
                                int *out_level) {
  irq_flags_t flags;
  uint64_t entry = __atomic_load_n(&current_table[index], __ATOMIC_ACQUIRE);

  if (entry & PTE_PRESENT) {
    if (entry & PTE_HUGE) {
      if (!alloc) {
        if (out_level) *out_level = level;
        return &current_table[index];
      }
      vmm_lock_table(current_table, &flags);
      /* Re-check under lock */
      entry = current_table[index];
      if (likely((entry & PTE_PRESENT) && (entry & PTE_HUGE))) {
        if (vmm_split_huge_page(mm, current_table, index, level, virt, nid) < 0) {
          vmm_unlock_table(current_table, flags);
          return NULL;
        }
      }
      entry = current_table[index];
      vmm_unlock_table(current_table, flags);
    }
    return (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }

  if (!alloc) {
    return NULL;
  }

  vmm_lock_table(current_table, &flags);
  /* Re-check after acquiring lock to prevent double allocation */
  entry = current_table[index];
  if (entry & PTE_PRESENT) {
    vmm_unlock_table(current_table, flags);
    return get_next_level(mm, current_table, index, alloc, level, virt, nid, out_level);
  }

  uint64_t new_table_phys = vmm_alloc_table_node(nid);
  if (!new_table_phys) {
    vmm_unlock_table(current_table, flags);
    return NULL;
  }

  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  uint64_t new_entry = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  __atomic_store_n(&current_table[index], new_entry, __ATOMIC_RELEASE);

  vmm_unlock_table(current_table, flags);
  return (uint64_t *) phys_to_virt(new_table_phys);
}

static uint64_t *vmm_get_pte_ptr(struct mm_struct *mm, uint64_t virt, bool alloc, int nid, int *out_level) {
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
      default: return NULL;
    }

    int next_out_level = 0;
    uint64_t *next_table = get_next_level(mm, current_table, index, alloc, level, virt, nid, &next_out_level);
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

  /* 
   * Decrement refcount for the extra mappings we just merged.
   * We replaced 512 mappings with 1 huge mapping.
   */
  struct page *base_pg = phys_to_page(base_phys);
  if (base_pg) {
    for (int i = 0; i < 511; i++) {
      put_page(base_pg);
    }
  }

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

  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  int ret = vmm_split_huge_page(mm, current_table, idx, target_level, virt, nid);
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
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return 0;
  /*
   * OPTIMIZATION: Lockless read of dirty bit.
   * Same rationale as vmm_is_accessed - reading is atomic on x86_64.
   */
  uint64_t entry = __atomic_load_n(pte_p, __ATOMIC_ACQUIRE);
  return (entry & PTE_DIRTY) ? 1 : 0;
}

void vmm_clear_dirty(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  int level = 0;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, &level);
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
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return 0;
  /*
   * OPTIMIZATION: Lockless read of accessed bit.
   * Reading a single bit is atomic on x86_64 and doesn't require locking.
   * The CPU sets this bit atomically, so we just need an acquire fence.
   */
  uint64_t entry = __atomic_load_n(pte_p, __ATOMIC_ACQUIRE);
  return (entry & PTE_ACCESSED) ? 1 : 0;
}

void vmm_clear_accessed(struct mm_struct *mm, uint64_t virt) {
  vmm_clear_accessed_no_flush(mm, virt);
  int level = 0;
  vmm_get_pte_ptr(mm, virt, false, mm ? mm->preferred_node : -1, &level);
  uint64_t size = (level == 2) ? VMM_PAGE_SIZE_2M : ((level == 3) ? VMM_PAGE_SIZE_1G : PAGE_SIZE);
  vmm_tlb_shootdown(mm, virt & ~(size - 1), (virt & ~(size - 1)) + size);
}

/**
 * vmm_clear_accessed_no_flush - Clear the accessed bit without TLB shootdown.
 *
 * OPTIMIZATION: For batched operations (like folio_referenced scanning multiple
 * mappings), we clear the accessed bit without flushing. The caller is responsible
 * for a single batched TLB shootdown at the end.
 *
 * This avoids O(n) TLB shootdowns when scanning n mappings of a folio.
 */
void vmm_clear_accessed_no_flush(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, NULL);
  if (!pte_p) return;
  /*
   * OPTIMIZATION: Use atomic AND to clear bit without lock.
   * On x86_64, the CPU updates A/D bits atomically, so we can use
   * atomic_and to clear without holding the page table lock.
   * This is safe because:
   * 1. Only the CPU can SET the accessed bit
   * 2. We only CLEAR it (no read-modify-write race on the bit itself)
   */
  __atomic_and_fetch(pte_p, ~PTE_ACCESSED, __ATOMIC_RELEASE);
}

int vmm_set_flags(struct mm_struct *mm, uint64_t virt, uint64_t flags) {
  if (!mm) mm = &init_mm;
  int level = 0;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, &level);
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
                                    uint64_t page_size, int nid, bool flush) {
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
    uint64_t *next_table = get_next_level(mm, current_table, index, true, level, virt, nid, NULL);
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

  /* Increment reference count for managed memory */
  struct page *pg = phys_to_page(phys);
  if (pg) {
    get_page(pg);
  }

  if (flush) {
    vmm_tlb_shootdown(mm, virt & ~(page_size - 1), (virt & ~(page_size - 1)) + page_size);
  }
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
      if (vmm_split_huge_page(mm, current_table, index, level, virt, nid) < 0) return 0;
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
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();
  return vmm_map_huge_page_locked(mm, virt, phys, flags, page_size, nid, true);
}

int vmm_map_huge_page_no_flush(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags, uint64_t page_size) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();
  return vmm_map_huge_page_locked(mm, virt, phys, flags, page_size, nid, false);
}

int vmm_map_page(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags) {
  return vmm_map_huge_page(mm, virt, phys, flags, VMM_PAGE_SIZE_4K);
}

int vmm_map_page_no_flush(struct mm_struct *mm, uint64_t virt, uint64_t phys, uint64_t flags) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();
  return vmm_map_huge_page_locked(mm, virt, phys, flags, VMM_PAGE_SIZE_4K, nid, false);
}

static int __vmm_map_pages_internal(struct mm_struct *mm, uint64_t virt, uint64_t phys, size_t count, uint64_t flags,
                                    bool flush) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  uint64_t start_virt = virt;
  uint64_t end_virt = virt + count * PAGE_SIZE;
  uint64_t *pml4 = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
  int levels = vmm_get_paging_levels(); // 4 or 5

  while (virt < end_virt) {
    uint64_t pml4_idx = (levels == 5) ? PML5_INDEX(virt) : PML4_INDEX(virt);
    uint64_t *pdpt;

    /* Level 4/5 Traversal */
    if (levels == 5) {
      // Handle PML5 if present (not fully implemented in this optimized path for brevity, assuming 4 level for now or handling gracefully)
      // For strictness, if levels==5, pml4 is PML5. We need to go down to PML4.
      uint64_t pml5_entry = pml4[pml4_idx];
      if (!(pml5_entry & PTE_PRESENT)) {
        uint64_t new_table = vmm_alloc_table_node(nid);
        if (!new_table) return -ENOMEM;
        pml4[pml4_idx] = new_table | PTE_PRESENT | PTE_RW | PTE_USER;
        pdpt = (uint64_t *) phys_to_virt(new_table); // This is actually PML4
      } else {
        pdpt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pml5_entry));
      }
      // Now pdpt is PML4. We need to go deeper.
      // Re-calculate index for PML4
      pml4_idx = PML4_INDEX(virt);
      uint64_t *pml4_next = pdpt;
      uint64_t pml4_entry = pml4_next[pml4_idx];
      if (!(pml4_entry & PTE_PRESENT)) {
        uint64_t new_table = vmm_alloc_table_node(nid);
        if (!new_table) return -ENOMEM;
        pml4_next[pml4_idx] = new_table | PTE_PRESENT | PTE_RW | PTE_USER;
        pdpt = (uint64_t *) phys_to_virt(new_table); // Now this is PDPT
      } else {
        pdpt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pml4_entry));
      }
    } else {
      // 4-level paging
      uint64_t pml4_entry = pml4[pml4_idx];
      if (!(pml4_entry & PTE_PRESENT)) {
        uint64_t new_table = vmm_alloc_table_node(nid);
        if (!new_table) return -ENOMEM;
        pml4[pml4_idx] = new_table | PTE_PRESENT | PTE_RW | PTE_USER;
        pdpt = (uint64_t *) phys_to_virt(new_table);
      } else {
        pdpt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pml4_entry));
      }
    }

    // Correct boundary calculation for loop
    // Each PML4 entry covers 512GB.
    // Each PDPT entry covers 1GB.

    // We are now at PDPT level.
    uint64_t pdpt_step = 1ULL << 30; // 1GB
    uint64_t loop_end_pdpt = (end_virt < ((virt & ~(0x8000000000ULL - 1)) + 0x8000000000ULL))
                               ? end_virt
                               : ((virt & ~(0x8000000000ULL - 1)) + 0x8000000000ULL);

    while (virt < loop_end_pdpt) {
      uint64_t pdpt_idx = PDPT_INDEX(virt);
      uint64_t pdpt_entry = pdpt[pdpt_idx];

      /* Try 1GB Huge Page */
      if (vmm_page_size_supported(VMM_PAGE_SIZE_1G) &&
          (virt & (VMM_PAGE_SIZE_1G - 1)) == 0 &&
          (phys & (VMM_PAGE_SIZE_1G - 1)) == 0 &&
          (virt + VMM_PAGE_SIZE_1G <= end_virt)) {
        uint64_t entry_val = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_HUGE | PTE_PRESENT;
        if (entry_val & PTE_PAT) {
          entry_val &= ~PTE_PAT;
          entry_val |= PDE_PAT;
        }

        struct page *pg = phys_to_page(pmm_virt_to_phys(pdpt));
        irq_flags_t f = spinlock_lock_irqsave(&pg->ptl);
        pdpt[pdpt_idx] = entry_val;
        spinlock_unlock_irqrestore(&pg->ptl, f);

        virt += VMM_PAGE_SIZE_1G;
        phys += VMM_PAGE_SIZE_1G;
        continue;
      }

      uint64_t *pd;
      if (!(pdpt_entry & PTE_PRESENT) || (pdpt_entry & PTE_HUGE)) {
        // If huge, we implicitly split by overwriting with a table?
        // Real implementation should split properly.
        // For strict correctness in this "optimization", if we encounter a huge page that blocks us,
        // we should probably split it.
        if (pdpt_entry & PTE_HUGE) {
          if (vmm_split_huge_page(mm, pdpt, pdpt_idx, 3, virt, nid) < 0) return -ENOMEM;
          pdpt_entry = pdpt[pdpt_idx];
        }

        if (!(pdpt_entry & PTE_PRESENT)) {
          uint64_t new_table = vmm_alloc_table_node(nid);
          if (!new_table) return -ENOMEM;
          struct page *pg = phys_to_page(pmm_virt_to_phys(pdpt));
          irq_flags_t f = spinlock_lock_irqsave(&pg->ptl);
          
          /* Re-check under lock */
          if (pdpt[pdpt_idx] & PTE_PRESENT) {
            spinlock_unlock_irqrestore(&pg->ptl, f);
            pmm_free_page(new_table);
            pdpt_entry = pdpt[pdpt_idx];
            pd = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pdpt_entry));
          } else {
            pdpt[pdpt_idx] = new_table | PTE_PRESENT | PTE_RW | PTE_USER;
            spinlock_unlock_irqrestore(&pg->ptl, f);
            pd = (uint64_t *) phys_to_virt(new_table);
          }
        } else {
          pd = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pdpt_entry));
        }
      } else {
        pd = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pdpt_entry));
      }

      // We are now at PD level.
      uint64_t pd_step = 1ULL << 21; // 2MB
      uint64_t loop_end_pd = (end_virt < ((virt & ~(pdpt_step - 1)) + pdpt_step))
                               ? end_virt
                               : ((virt & ~(pdpt_step - 1)) + pdpt_step);

      while (virt < loop_end_pd) {
        uint64_t pd_idx = PD_INDEX(virt);
        uint64_t pd_entry = pd[pd_idx];

        /* Try 2MB Huge Page */
        if (vmm_page_size_supported(VMM_PAGE_SIZE_2M) &&
            (virt & (VMM_PAGE_SIZE_2M - 1)) == 0 &&
            (phys & (VMM_PAGE_SIZE_2M - 1)) == 0 &&
            (virt + VMM_PAGE_SIZE_2M <= end_virt)) {
          uint64_t entry_val = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_HUGE | PTE_PRESENT;
          if (entry_val & PTE_PAT) {
            entry_val &= ~PTE_PAT;
            entry_val |= PDE_PAT;
          }

          struct page *pg = phys_to_page(pmm_virt_to_phys(pd));
          irq_flags_t f = spinlock_lock_irqsave(&pg->ptl);
          pd[pd_idx] = entry_val;
          spinlock_unlock_irqrestore(&pg->ptl, f);

          virt += VMM_PAGE_SIZE_2M;
          phys += VMM_PAGE_SIZE_2M;
          continue;
        }

        uint64_t *pt;
        if (!(pd_entry & PTE_PRESENT) || (pd_entry & PTE_HUGE)) {
          if (pd_entry & PTE_HUGE) {
            if (vmm_split_huge_page(mm, pd, pd_idx, 2, virt, nid) < 0) return -ENOMEM;
            pd_entry = pd[pd_idx];
          }
          if (!(pd_entry & PTE_PRESENT)) {
            uint64_t new_table = vmm_alloc_table_node(nid);
            if (!new_table) return -ENOMEM;
            struct page *pg = phys_to_page(pmm_virt_to_phys(pd));
            irq_flags_t f = spinlock_lock_irqsave(&pg->ptl);

            /* Re-check under lock */
            if (pd[pd_idx] & PTE_PRESENT) {
              spinlock_unlock_irqrestore(&pg->ptl, f);
              pmm_free_page(new_table);
              pd_entry = pd[pd_idx];
              pt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pd_entry));
            } else {
              pd[pd_idx] = new_table | PTE_PRESENT | PTE_RW | PTE_USER;
              spinlock_unlock_irqrestore(&pg->ptl, f);
              pt = (uint64_t *) phys_to_virt(new_table);
            }
          } else {
            pt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pd_entry));
          }
        } else {
          pt = (uint64_t *) phys_to_virt(PTE_GET_ADDR(pd_entry));
        }

        // We are now at PT level.
        // Optimized leaf filling loop.
        uint64_t loop_end_pt = (end_virt < ((virt & ~(pd_step - 1)) + pd_step))
                                 ? end_virt
                                 : ((virt & ~(pd_step - 1)) + pd_step);

        struct page *pg = phys_to_page(pmm_virt_to_phys(pt));
        irq_flags_t f = spinlock_lock_irqsave(&pg->ptl);

        while (virt < loop_end_pt) {
          uint64_t pt_idx = PT_INDEX(virt);
          pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & ~PTE_ADDR_MASK) | PTE_PRESENT;
          virt += PAGE_SIZE;
          phys += PAGE_SIZE;
        }
        spinlock_unlock_irqrestore(&pg->ptl, f);
      }
    }
  }

  if (flush) {
    vmm_tlb_shootdown(mm, start_virt, end_virt);
  }
  return 0;
}

int vmm_map_pages(struct mm_struct *mm, uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
  return __vmm_map_pages_internal(mm, virt, phys, count, flags, true);
}

int vmm_map_pages_no_flush(struct mm_struct *mm, uint64_t virt, uint64_t phys, size_t count, uint64_t flags) {
  return __vmm_map_pages_internal(mm, virt, phys, count, flags, false);
}

int vmm_map_page_array_no_flush(struct mm_struct *mm, uint64_t virt, struct page **pages, size_t count, uint64_t flags) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  uint64_t entry_flags = flags & ~PTE_ADDR_MASK;
  if (entry_flags & PDE_PAT) {
    entry_flags &= ~PDE_PAT;
    entry_flags |= PTE_PAT;
  }
  entry_flags |= PTE_PRESENT;

  while (count > 0) {
     int level = 0;
     uint64_t *pte_ptr = vmm_get_pte_ptr(mm, virt, true, nid, &level);
     if (!pte_ptr) return -ENOMEM;
     
     if (level != 1) return -EINVAL;

     /* 
      * We have a pointer to the PTE entry. 
      * We can fill the rest of this Page Table (up to 512 entries) without re-walking.
      */
     uint64_t pt_index = PT_INDEX(virt);
     int batch = min((size_t)(512 - pt_index), count);
     
     struct page *pt_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_ptr & PAGE_MASK)));
     irq_flags_t f = spinlock_lock_irqsave(&pt_page->ptl);
     
     /* Re-read pte_ptr under lock because vmm_get_pte_ptr might have returned a stale one if racing,
      * though in vmalloc we are usually the only ones mapping this range. */
     uint64_t *pte_table = (uint64_t*)((uint64_t)pte_ptr & PAGE_MASK);
     
     for (int i = 0; i < batch; i++) {
         uint64_t phys = page_to_phys(pages[i]);
         pte_table[pt_index + i] = (phys & PTE_ADDR_MASK) | entry_flags;
     }
     spinlock_unlock_irqrestore(&pt_page->ptl, f);
     
     virt += (uint64_t)batch * PAGE_SIZE;
     pages += batch;
     count -= batch;
  }
  return 0;
}

int vmm_map_pages_list(struct mm_struct *mm, uint64_t virt, const uint64_t *phys_list, size_t count, uint64_t flags) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  uint64_t start_virt = virt;
  uint64_t total = count;

  uint64_t entry_flags = flags & ~PTE_ADDR_MASK;
  if (entry_flags & PDE_PAT) {
    entry_flags &= ~PDE_PAT;
    entry_flags |= PTE_PAT;
  }
  entry_flags |= PTE_PRESENT;

  while (count > 0) {
     int level = 0;
     uint64_t *pte = vmm_get_pte_ptr(mm, virt, true, nid, &level);
     if (!pte) return -ENOMEM;
     
     if (level != 1) return -EINVAL;

     uint64_t idx = PT_INDEX(virt);
     int batch = min((size_t)(512 - idx), count);
     
     struct page *pt_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte & PAGE_MASK)));
     irq_flags_t f = spinlock_lock_irqsave(&pt_page->ptl);
     
     for (int i=0; i < batch; i++) {
         pte[i] = (phys_list[i] & PTE_ADDR_MASK) | entry_flags;
     }
     spinlock_unlock_irqrestore(&pt_page->ptl, f);
     
     virt += batch * PAGE_SIZE;
     phys_list += batch;
     count -= batch;
  }
  vmm_tlb_shootdown(mm, start_virt, start_virt + total * PAGE_SIZE);
  return 0;
}

uint64_t vmm_unmap_page_no_flush(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();
  return vmm_unmap_page_locked(mm, virt, nid);
}

struct folio *vmm_unmap_folio_no_flush(struct mm_struct *mm, uint64_t virt) {
  uint64_t phys = vmm_unmap_page_no_flush(mm, virt);
  if (!phys) return NULL;
  struct page *page = phys_to_page(phys);
  if (!page) return NULL;
  return page_folio(page);
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
      if (level == 3) return (entry & 0x000FFFFFC0000000ULL) + (virt & 0x3FFFFFFF);
      if (level == 2) return (entry & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
      return 0;
    }
    current_table = (uint64_t *) phys_to_virt(PTE_GET_ADDR(entry));
  }
  uint64_t entry = current_table[PT_INDEX(virt)];
  if (!(entry & PTE_PRESENT)) return 0;
  return PTE_GET_ADDR(entry) + (virt & 0xFFF);
}

static int vmm_copy_level(uint64_t src_table_phys, uint64_t dst_table_phys, int level, int nid) {
  uint64_t *src_table = (uint64_t *) phys_to_virt(src_table_phys);
  uint64_t *dst_table = (uint64_t *) phys_to_virt(dst_table_phys);
  int entries = (level == vmm_get_paging_levels()) ? 256 : 512;
  for (int i = 0; i < entries; i++) {
    uint64_t entry = src_table[i];
    if (!(entry & PTE_PRESENT)) continue;
    if (level > 1 && !(entry & PTE_HUGE)) {
      uint64_t new_table_phys = vmm_alloc_table_node(nid);
      if (!new_table_phys) return -ENOMEM;
      dst_table[i] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
      int res = vmm_copy_level(PTE_GET_ADDR(entry), new_table_phys, level - 1, nid);
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
  int nid = dst_mm->preferred_node;
  if (nid == -1) nid = this_node();
  return vmm_copy_level((uint64_t) src_mm->pml_root, (uint64_t) dst_mm->pml_root, vmm_get_paging_levels(),
                        nid);
}

int vmm_handle_cow(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, mm->preferred_node, NULL);
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

  int nid = mm->preferred_node;
  if (nid == -1) nid = this_node();

  struct folio *new_folio = alloc_pages_node(nid, GFP_KERNEL, 0);
  if (!new_folio) {
    put_page(old_page);
    return -ENOMEM;
  }
  uint64_t new_phys = folio_to_phys(new_folio);

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

void vmm_dump_entry(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *current_table = (uint64_t *) phys_to_virt((uint64_t) mm->pml_root);
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
      printk(VMM_CLASS "  Level %d missing\n", level);
      return;
    }
    if (entry & PTE_HUGE) {
      printk(VMM_CLASS "  Level %d: HUGE PAGE, entry: %llx\n", level, entry);
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
  if (!features->pdpe1gb) printk(KERN_NOTICE VMM_CLASS "1GB pages not supported\n");
  g_kernel_pml_root = vmm_alloc_table();
  if (!g_kernel_pml_root) panic(VMM_CLASS "Failed to allocate kernel PML root");

  g_support_1gb = features->pdpe1gb;

  uint64_t boot_pml_root_phys;
  __asm__ volatile("mov %%cr3, %0" : "=r"(boot_pml_root_phys));
  boot_pml_root_phys &= PTE_ADDR_MASK;
  uint64_t *boot_pml_root = (uint64_t *) phys_to_virt(boot_pml_root_phys);
  uint64_t *kernel_pml_root = (uint64_t *) phys_to_virt(g_kernel_pml_root);
  int levels = vmm_get_paging_levels();

  /*
   * AeroSync constructs its own kernel page tables to gain full control
   * and independence from the bootloader's initial setup.
   */
  for (int i = 256; i < 512; i++) {
    kernel_pml_root[i] = boot_pml_root[i];
  }

  mm_init(&init_mm);
  init_mm.pml_root = (uint64_t *) g_kernel_pml_root;

  /*
   * Explicitly map the HHDM (Direct Map) to ensure optimal page sizes (2MB/1GB)
   * and consistent attributes across the entire physical address space.
   * This also ensures that Limine modules (which live in HHDM) are correctly mapped.
   */
  uint64_t max_pfn = pmm_get_max_pfn();
  for (uint64_t pfn = 0; pfn < max_pfn; pfn += 512) {
      uint64_t virt = HHDM_VIRT_BASE + (pfn << 12);
      vmm_map_huge_page_no_flush(&init_mm, virt, pfn << 12, PTE_PRESENT | PTE_RW | PTE_GLOBAL, VMM_PAGE_SIZE_2M);
  }

  vmm_switch_pml_root(g_kernel_pml_root);
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
  if (*(uint8_t *) phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt)) != 0xAA)
    panic(
      "VMM Stress: Parent data mismatch");
  if (*(uint8_t *) phys_to_virt(vmm_virt_to_phys(child_mm, cow_virt)) != 0xAA) panic("VMM Stress: Child data mismatch");

  /* Trigger COW in parent */
  vmm_handle_cow(parent_mm, cow_virt);
  uint8_t *parent_ptr = (uint8_t *) phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt));
  *parent_ptr = 0xBB;

  /* Verify Isolation */
  if (*(uint8_t *) phys_to_virt(vmm_virt_to_phys(parent_mm, cow_virt)) != 0xBB) panic("VMM Stress: Parent COW failed");
  if (*(uint8_t *) phys_to_virt(vmm_virt_to_phys(child_mm, cow_virt)) != 0xAA)
    panic(
      "VMM Stress: Child corrupted by parent!");

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
  if (check_phys == 0 || *(uint8_t *) phys_to_virt(check_phys) != 0xCC) panic("VMM Stress: Shatter corrupted data");

  if (vmm_virt_to_phys(hp_mm, hp_virt + 128 * PAGE_SIZE) != 0) panic("VMM Stress: Unmap in huge page failed");

  mm_destroy(hp_mm);
  printk(KERN_DEBUG VMM_CLASS "  - Huge Page Shatter Stress: OK\n");

  printk(KERN_DEBUG VMM_CLASS "VMM Production Stress Test Passed.\n");
}

/**
 * NUMA Hinting Support
 */

int vmm_is_numa_hint(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, -1, NULL);
  if (!pte_p) return 0;
  
  uint64_t entry = *pte_p;
  /* NUMA hint: NOT present, but PTE_NUMA_HINT bit set */
  return (!(entry & PTE_PRESENT) && (entry & PTE_NUMA_HINT));
}

struct folio *vmm_get_folio(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, -1, NULL);
  if (!pte_p) return NULL;
  
  uint64_t entry = *pte_p;
  if (!(entry & (PTE_PRESENT | PTE_NUMA_HINT))) return NULL;
  
  uint64_t phys = PTE_GET_ADDR(entry);
  struct page *page = phys_to_page(phys);
  if (!page) return NULL;
  
  struct folio *folio = page_folio(page);
  folio_get(folio);
  return folio;
}

void vmm_set_numa_hint(struct mm_struct *mm, uint64_t virt) {
  if (!mm) mm = &init_mm;
  uint64_t *pte_p = vmm_get_pte_ptr(mm, virt, false, -1, NULL);
  if (!pte_p) return;
  
  struct page *table_page = phys_to_page(pmm_virt_to_phys((void *)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  
  uint64_t entry = *pte_p;
  if (entry & PTE_PRESENT) {
      /* Clear present, set hint bit */
      entry &= ~PTE_PRESENT;
      entry |= PTE_NUMA_HINT;
      *pte_p = entry;
  }
  
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  /* We MUST flush TLB so the CPU sees the 'not present' state */
  vmm_tlb_shootdown(mm, virt, virt + PAGE_SIZE);
}
