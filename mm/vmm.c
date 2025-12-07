#include <arch/x64/cpu.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <kernel/spinlock.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/pmm.h>

// Global kernel PML4 (physical address)
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

static uint64_t *get_next_level(uint64_t *current_table, uint64_t index,
                                bool alloc) {
  uint64_t entry = current_table[index];

  if (entry & PTE_PRESENT) {
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

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), true);
  if (!pdpt)
    return -1;

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), true);
  if (!pd)
    return -1;

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), true);
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

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), false);
  if (!pdpt)
    return 0;

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), false);
  if (!pd)
    return 0;

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), false);
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

  uint64_t *pdpt = get_next_level(pml4, PML4_INDEX(virt), false);
  if (!pdpt)
    return 0;

  uint64_t *pd = get_next_level(pdpt, PDPT_INDEX(virt), false);
  if (!pd)
    return 0;

  uint64_t *pt = get_next_level(pd, PD_INDEX(virt), false);
  if (!pt)
    return 0;

  uint64_t entry = pt[PT_INDEX(virt)];
  if (!(entry & PTE_PRESENT))
    return 0;

  return PTE_GET_ADDR(entry) + (virt & (PAGE_SIZE - 1));
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
  for (int i = 256; i < 512; i++) {
    kernel_pml4[i] = boot_pml4[i];
  }

  // Reload CR3
  vmm_switch_pml4(g_kernel_pml4);

  printk(VMM_CLASS "VMM Initialized and switched to new Page Table.\n");
}
