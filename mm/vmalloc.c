#include <arch/x64/mm/layout.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <mm/vmalloc.h>
#include <mm/vma.h>

/*
 * Ranges are defined in arch/x64/mm/layout.h
 */

static uint64_t g_vmalloc_virt_next = VMALLOC_VIRT_BASE;
static spinlock_t vmalloc_lock = 0;

/* Using standard PAGE_SIZE from PMM */

void *vmalloc(size_t size) {
  if (size == 0)
    return NULL;

  /* Align size to page boundary */
  size_t aligned_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
  size_t pages_needed = aligned_size / PAGE_SIZE;

  /* 1. Allocate Virtual Range */
  /* TODO: Use `vma_create` / `vma_insert` into `init_mm` properly.
     For now, we use a bump allocator similar to slab for simplicity
     until we have a smarter allocator for the VMA hole search.
     The VMA system tracks it, but we need to pick an address. */

  spinlock_lock(&vmalloc_lock);

  // Find a free virtual memory region
  uint64_t virt_start =
      vma_find_free_region(&init_mm, size, VMALLOC_VIRT_BASE, VMALLOC_VIRT_END);

  if (!virt_start) {
    spinlock_unlock(&vmalloc_lock);
    printk(KERN_ERR KERN_CLASS
           "vmalloc: No virtual memory available (size: %zu)\n",
           size);
    return NULL;
  }

  // Create VMA entry to track this region in the RB tree
  struct vm_area_struct *vma =
      vma_create(virt_start, virt_start + aligned_size, VM_READ | VM_WRITE);
  if (!vma) {
    spinlock_unlock(&vmalloc_lock);
    return NULL;
  }

  if (vma_insert(&init_mm, vma) != 0) {
    // Collision should not happen if vma_find_free_region works correctly
    printk(KERN_ERR KERN_CLASS "vmalloc: unexpected collision at %p\n",
           (void *)virt_start);
    vma_free(vma);
    spinlock_unlock(&vmalloc_lock);
    return NULL;
  }
  spinlock_unlock(&vmalloc_lock);

  /* 2. Allocate and Map Physical Pages */
  for (size_t i = 0; i < pages_needed; i++) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
      // TODO: Unwind!
      panic(KERN_CLASS "vmalloc: OOM during allocation");
    }

    uint64_t virt_addr = virt_start + i * PAGE_SIZE;
    // Use global kernel PML4 (physical) for mapping kernel vmalloc memory
    vmm_map_page(g_kernel_pml4, virt_addr, phys, PTE_PRESENT | PTE_RW | PTE_NX);
  }

  return (void *)virt_start;
}

void vfree(void *ptr) {
  if (!ptr)
    return;

  uint64_t virt = (uint64_t)ptr;

  struct vm_area_struct *vma = vma_find(&init_mm, virt);
  if (!vma || vma->vm_start != virt) {
    printk(KERN_ERR KERN_CLASS "vfree: Invalid pointer %p\n", ptr);
    return;
  }

  // Unmap and free pages
  uint64_t size = vma->vm_end - vma->vm_start;
  size_t pages = size / PAGE_SIZE;

  for (size_t i = 0; i < pages; i++) {
    uint64_t vaddr = virt + i * PAGE_SIZE;
    // We need a vmm_unmap that returns the phys addr to free it
    // For now, vmm_virt_to_phys?
    uint64_t phys = vmm_virt_to_phys(g_kernel_pml4, vaddr);
    if (phys) {
      pmm_free_page(phys);
      vmm_unmap_page(g_kernel_pml4, vaddr);
    }
  }

  // Remove VMA and free it
  vma_remove(&init_mm, vma);
  vma_free(vma);
}
