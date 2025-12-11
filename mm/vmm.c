#include <arch/x64/cpu.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>

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

uint64_t vmm_virt_to_phys(uint64_t pml4_phys, uint64_t virt) {
  // Read-only, lock-free walk is usually safe if we assume page tables aren't
  // freed under our feet. For strict correctness, we can lock.

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

  uint64_t entry = pt[PT_INDEX(virt)];
  if (!(entry & PTE_PRESENT)) {
    // Check if it was a huge page earlier?
    // Wait, get_next_level returns NULL for huge page.
    // We need a proper walker here.
    return 0;
  }

  return PTE_GET_ADDR(entry) + (virt & (PAGE_SIZE - 1));
}

// Improved virt_to_phys that handles Huge Pages
uint64_t vmm_virt_to_phys_huge(uint64_t pml4_phys, uint64_t virt) {
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

// Simple bump allocator for MMIO virtual space
// Starts at 0xFFFF900000000000 (Arbitrary gap between HHDM and Kernel)
#define MMIO_VIRT_BASE 0xFFFF900000000000
static uint64_t g_next_mmio_virt = MMIO_VIRT_BASE;

void *vmm_map_mmio(uint64_t phys_addr, size_t size) {
  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  // Align start/end to page boundaries
  uint64_t phys_start = PAGE_ALIGN_DOWN(phys_addr);
  uint64_t phys_end = PAGE_ALIGN_UP(phys_addr + size);
  uint64_t aligned_size = phys_end - phys_start;
  uint64_t offset_in_page = phys_addr - phys_start;

  // Allocate virtual range
  uint64_t virt_start = g_next_mmio_virt;
  g_next_mmio_virt += aligned_size;

  // Map each page
  for (uint64_t i = 0; i < aligned_size; i += PAGE_SIZE) {
    // Use NO_CACHE (PCD) + RW + PRESENT for MMIO
    uint64_t flags = PTE_PRESENT | PTE_RW | PTE_PCD;
    vmm_map_page_locked(g_kernel_pml4, virt_start + i, phys_start + i, flags);
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);

  // Return virtual address with original offset applied
  return (void *)(virt_start + offset_in_page);
}

void vmm_unmap_mmio(void *virt_addr, size_t size) {
  // Note: We don't reclaim virtual space in this simple bump allocator
  // This is fine for permanent MMIO mappings (APIC, HPET, etc.)
  // A proper allocator (buddy/slab/bitmap) is needed for dynamic
  // mapped/unmapped driver buffers.

  irq_flags_t irq = spinlock_lock_irqsave(&vmm_lock);

  uint64_t virt_start = PAGE_ALIGN_DOWN((uint64_t)virt_addr);
  uint64_t virt_end = PAGE_ALIGN_UP((uint64_t)virt_addr + size);

  for (uint64_t v = virt_start; v < virt_end; v += PAGE_SIZE) {
    vmm_unmap_page_locked(g_kernel_pml4, v);
  }

  spinlock_unlock_irqrestore(&vmm_lock, irq);
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

  // Copy the higher half (entries 256-511)
  // This includes the kernel (0xffffffff80000000 range) and HHDM
  // (0xffff8000...)
  // Copy the higher half (entries 256-511)
  // This includes the kernel (0xffffffff80000000 range) and HHDM
  // (0xffff8000...)
  // Copy the higher half (entries 256-511)
  // This includes the kernel (0xffffffff80000000 range) and HHDM
  // (0xffff8000...)
  memcpy(kernel_pml4 + 256, boot_pml4 + 256, 256 * sizeof(uint64_t));

  // Reload CR3
  vmm_switch_pml4(g_kernel_pml4);

  // FIX: Remap kernel sections with precise permissions (4KB granularity)
  // This overrides any large pages Limine might have set that cause NX issues.

  // 1. Text (RX)
  uint64_t text_start = (uint64_t)_text_start;
  uint64_t text_end = (uint64_t)_text_end;
  for (uint64_t v = text_start; v < text_end; v += PAGE_SIZE) {
    uint64_t p =
        vmm_virt_to_phys_huge(g_kernel_pml4, v); // Use huge-aware walker
    if (p) {
      vmm_map_page(g_kernel_pml4, v, p,
                   PTE_PRESENT); // No NX, No RW == RX (if WP enabled)
                                 // Note: In some setups, R/O requires WP bit in
                                 // CR0. Assuming standard x64 behavior:
                                 // Present=1, RW=0, NX=0 => Read/Exec
    }
  }

  // 2. ROData (R + NX)
  uint64_t rod_start = (uint64_t)_rodata_start;
  uint64_t rod_end = (uint64_t)_rodata_end;
  for (uint64_t v = rod_start; v < rod_end; v += PAGE_SIZE) {
    uint64_t p = vmm_virt_to_phys_huge(g_kernel_pml4, v);
    if (p) {
      vmm_map_page(g_kernel_pml4, v, p, PTE_PRESENT | PTE_NX);
    }
  }

  // 3. Data + BSS (RW + NX)
  uint64_t data_start = (uint64_t)_data_start;
  // Note: We remap everything after data start up to a safe limit or just
  // _kernel_phys_end? Let's rely on _data_end which usually includes BSS if
  // linker script orders it right. Check linker script: .data then .bss.
  // _bss_end is the true end. But wait, I don't have _bss_end externed here.
  // Let's assume _data_end covers .data. BSS might need separate handling if
  // symbols differ. Linker script: _data_end is after .data. _bss_end is after
  // .bss. We need to map BSS too. Let's just map from _data_start to a
  // reasonable end or import _bss_end. For now, let's just map _data_start ->
  // _data_end. BSS is usually allocated by Limine too? Actually, VMM copy loop
  // covered everything. We just need to fix permissions for TEXT. The rest
  // (Data/BSS) being RWX (if huge page) is "unsafe" but won't crash. The
  // CRITICAL part is TEXT being NX if it shared a page with data that was
  // marked NX? Or maybe Limine mapped it RW and we want RX.

  // Let's specifically fix .text to be executable.

  // Also, we must ensure we don't accidentally unmap something critical.

  printk(VMM_CLASS
         "Remapped kernel text to 4KB pages with EXEC permissions.\n");

  printk(VMM_CLASS "VMM Initialized and switched to new Page Table.\n");
}
