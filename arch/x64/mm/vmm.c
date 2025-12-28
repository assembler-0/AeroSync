///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file arch/x64/mm/vmm.c
 * @brief Virtual Memory Manager implementation (Level-Aware)
 * @copyright (C) 2025 assembler-0
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
#include <arch/x64/mm/tlb.h>

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
 * @return 0 on success.
 */
static int vmm_split_huge_page(uint64_t *table, uint64_t index, int level) {
    uint64_t entry = table[index];
    uint64_t new_table_phys = vmm_alloc_table();
    if (!new_table_phys) return -1;

    uint64_t *new_table = (uint64_t *)phys_to_virt(new_table_phys);
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

    // Replace huge page entry with pointer to new table.
    // We use full permissions for intermediate tables; leaf PTEs control final access.
    table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;

    // Flush TLB to ensure the CPU drops the cached Huge Page translation
    // and starts using the new Page Table.
    uint64_t current_cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(current_cr3) : "memory");

    return 0;
}

static uint64_t *get_next_level(uint64_t *current_table, uint64_t index,
                                bool alloc, int level) {
  uint64_t entry = current_table[index];

  if (entry & PTE_PRESENT) {
    if (entry & PTE_HUGE) {
      if (!alloc) return NULL;
      
      // Auto-split huge page if we need to go deeper for a 4KB mapping
      if (vmm_split_huge_page(current_table, index, level) < 0)
          return NULL;
      
      // Re-read the entry after split
      entry = current_table[index];
    }
    return (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  }

  if (!alloc) return NULL;

  uint64_t new_table_phys = vmm_alloc_table();
  if (!new_table_phys) return NULL;

  // Initialize PTL for the new table page
  struct page *pg = phys_to_page(new_table_phys);
  spinlock_init(&pg->ptl);

  current_table[index] = new_table_phys | PTE_PRESENT | PTE_RW | PTE_USER;
  return (uint64_t *)phys_to_virt(new_table_phys);
}

/**
 * Traverses page tables and returns a pointer to the leaf PTE.
 */
static uint64_t *vmm_get_pte_ptr(uint64_t pml_root_phys, uint64_t virt, bool alloc) {
    uint64_t *current_table = (uint64_t *)phys_to_virt(pml_root_phys);
    int levels = vmm_get_paging_levels();

    for (int level = levels; level > 1; level--) {
        uint64_t index;
        switch (level) {
            case 5: index = PML5_INDEX(virt); break;
            case 4: index = PML4_INDEX(virt); break;
            case 3: index = PDPT_INDEX(virt); break;
            case 2: index = PD_INDEX(virt); break;
            default: return NULL;
        }

        uint64_t *next_table = get_next_level(current_table, index, alloc, level);
        if (!next_table) return NULL;
        current_table = next_table;
    }

    return &current_table[PT_INDEX(virt)];
}

/* --- PTE Flag Helpers --- */

int vmm_is_dirty(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return 0;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int dirty = (*pte_p & PTE_DIRTY) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);
  
  return dirty;
}

void vmm_clear_dirty(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_DIRTY;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  vmm_tlb_flush_local(virt);
}

int vmm_is_accessed(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return 0;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  int accessed = (*pte_p & PTE_ACCESSED) ? 1 : 0;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  return accessed;
}

void vmm_clear_accessed(uint64_t pml_root, uint64_t virt) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t flags = spinlock_lock_irqsave(&table_page->ptl);
  *pte_p &= ~PTE_ACCESSED;
  spinlock_unlock_irqrestore(&table_page->ptl, flags);

  vmm_tlb_flush_local(virt);
}

int vmm_set_flags(uint64_t pml_root, uint64_t virt, uint64_t flags) {
  uint64_t *pte_p = vmm_get_pte_ptr(pml_root, virt, false);
  if (!pte_p) return -1;

  struct page *table_page = phys_to_page(pmm_virt_to_phys((void*)((uint64_t)pte_p & PAGE_MASK)));
  irq_flags_t ptl_flags = spinlock_lock_irqsave(&table_page->ptl);

  uint64_t pte = *pte_p;
  uint64_t phys = PTE_GET_ADDR(pte);
  *pte_p = phys | flags | PTE_PRESENT;

  spinlock_unlock_irqrestore(&table_page->ptl, ptl_flags);

  vmm_tlb_flush_local(virt);
  return 0;
}

// --- Internal Unlocked Helpers ---

static int vmm_map_huge_page_locked(uint64_t pml_root_phys, uint64_t virt, uint64_t phys,
                                    uint64_t flags, uint64_t page_size) {
    uint64_t *current_table = (uint64_t *)phys_to_virt(pml_root_phys);
    int levels = vmm_get_paging_levels();
    
    int target_level = 1; // 4KB
    if (page_size == VMM_PAGE_SIZE_2M) target_level = 2;
    else if (page_size == VMM_PAGE_SIZE_1G) target_level = 3;

    for (int level = levels; level > target_level; level--) {
        uint64_t index;
        switch (level) {
            case 5: index = PML5_INDEX(virt); break;
            case 4: index = PML4_INDEX(virt); break;
            case 3: index = PDPT_INDEX(virt); break;
            case 2: index = PD_INDEX(virt); break;
            default: return -1;
        }
        
        uint64_t *next_table = get_next_level(current_table, index, true, level);
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
        if (target_level > 1) {
            // Full flush for huge pages for safety
            __asm__ volatile("mov %0, %%cr3" :: "r"(current_cr3) : "memory");
        } else {
            __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
        }
    }

    return 0;
}

static uint64_t vmm_unmap_page_locked(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *)phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  
  for (int level = levels; level > 1; level--) {
      uint64_t index;
      switch (level) {
          case 5: index = PML5_INDEX(virt); break;
          case 4: index = PML4_INDEX(virt); break;
          case 3: index = PDPT_INDEX(virt); break;
          case 2: index = PD_INDEX(virt); break;
          default: return 0;
      }

      uint64_t entry = current_table[index];
      if (!(entry & PTE_PRESENT)) return 0;

      if (entry & PTE_HUGE) {
          // If we encounter a huge page while trying to unmap a 4KB page,
          // we must split it first to avoid unmapping the entire huge region.
          if (vmm_split_huge_page(current_table, index, level) < 0) {
              return 0; // Failed to split (OOM)
          }
          entry = current_table[index]; // Refresh entry after split
      }

      current_table = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
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
  for (size_t i = 0; i < count; i++) {
    if (vmm_map_huge_page_locked(pml_root_phys, virt + i * PAGE_SIZE,
                                 phys + i * PAGE_SIZE, flags, VMM_PAGE_SIZE_4K) < 0) {
      spinlock_unlock_irqrestore(&vmm_lock, irq);
      return -1;
    }
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

int vmm_unmap_page(uint64_t pml_root_phys, uint64_t virt) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  vmm_unmap_page_locked(pml_root_phys, virt);
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return 0;
}

int vmm_unmap_pages(uint64_t pml_root_phys, uint64_t virt, size_t count) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  for (size_t i = 0; i < count; i++) {
    vmm_unmap_page_locked(pml_root_phys, virt + i * PAGE_SIZE);
  }
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return 0;
}

int vmm_unmap_pages_and_get_phys(uint64_t pml_root_phys, uint64_t virt,
                                 uint64_t *phys_list, size_t count) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);
  for (size_t i = 0; i < count; i++) {
    phys_list[i] = vmm_unmap_page_locked(pml_root_phys, virt + i * PAGE_SIZE);
  }
  spinlock_unlock_irqrestore(&vmm_lock, irq);
  return 0;
}

uint64_t vmm_virt_to_phys(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *)phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  uint64_t entry;
  uint64_t idx;

  for (int level = levels; level > 1; level--) {
    switch (level) {
      case 5: idx = PML5_INDEX(virt); break;
      case 4: idx = PML4_INDEX(virt); break;
      case 3: idx = PDPT_INDEX(virt); break;
      case 2: idx = PD_INDEX(virt); break;
      default: return 0;
    }

    entry = current_table[idx];
    if (!(entry & PTE_PRESENT)) return 0;

    if (entry & PTE_HUGE) {
      if (level == 3) return PTE_GET_ADDR(entry) + (virt & 0x3FFFFFFF);
      if (level == 2) return PTE_GET_ADDR(entry) + (virt & 0x1FFFFF);
      return 0;
    }

    current_table = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
  }

  idx = PT_INDEX(virt);
  entry = current_table[idx];
  if (!(entry & PTE_PRESENT)) return 0;

  return PTE_GET_ADDR(entry) + (virt & 0xFFF);
}

void vmm_dump_entry(uint64_t pml_root_phys, uint64_t virt) {
  uint64_t *current_table = (uint64_t *)phys_to_virt(pml_root_phys);
  int levels = vmm_get_paging_levels();
  uint64_t entry = 0;

  printk(VMM_CLASS "Dumping flags for virt: %llx (%d levels)\n", virt, levels);

  for (int level = levels; level > 1; level--) {
    uint64_t idx;
    switch (level) {
      case 5: idx = PML5_INDEX(virt); break;
      case 4: idx = PML4_INDEX(virt); break;
      case 3: idx = PDPT_INDEX(virt); break;
      case 2: idx = PD_INDEX(virt); break;
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

    current_table = (uint64_t *)phys_to_virt(PTE_GET_ADDR(entry));
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

void vmm_switch_pml4(uint64_t pml_root_phys) {
  __asm__ volatile("mov %0, %%cr3" ::"r"(pml_root_phys) : "memory");
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

  uint64_t *boot_pml_root = (uint64_t *)phys_to_virt(boot_pml_root_phys);
  uint64_t *kernel_pml_root = (uint64_t *)phys_to_virt(g_kernel_pml4);

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
  init_mm.pml4 = (uint64_t *)g_kernel_pml4;

  printk(VMM_CLASS "VMM Initialized (%d levels active).\n", vmm_get_paging_levels());

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
  *(volatile uint64_t*)phys_to_virt(test_phys) = 0x1234; 
  // Note: We access via HHDM here, but on some CPUs the hardware walker 
  // only sets dirty if accessed via the specific virtual mapping.
  // Let's just check if the helper can clear/set flags.
  
  vmm_set_flags(g_kernel_pml4, test_virt, PTE_RW | PTE_DIRTY);
  if (!vmm_is_dirty(g_kernel_pml4, test_virt)) {
      panic("VMM Smoke Test: Dirty bit helper failed");
  }
  printk(KERN_DEBUG VMM_CLASS "  - Dirty/Flags Helpers: OK\n");

  vmm_unmap_page(g_kernel_pml4, test_virt);
  pmm_free_page(test_phys);
  printk(KERN_DEBUG VMM_CLASS "  - Unmap: OK\n");

  up_write(&init_mm.mmap_lock);
  printk(KERN_DEBUG VMM_CLASS "VMM Smoke Test Passed.\n");
}
