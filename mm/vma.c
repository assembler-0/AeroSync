#include <arch/x64/mm/pmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <lib/rbtree.h>
#include <lib/string.h>
#include <mm/mmio.h>
#include <mm/slab.h>
#include <mm/vma.h>

#define BOOTSTRAP_VMA_COUNT 128
static struct vm_area_struct bootstrap_vmas[BOOTSTRAP_VMA_COUNT];
static bool
    bootstrap_in_use[BOOTSTRAP_VMA_COUNT]; // all false (0) by default in BSS
static spinlock_t bootstrap_lock = 0;

void mm_init(struct mm_struct *mm) {
  mm->mm_rb = RB_ROOT;
  mm->mmap = NULL;
  mm->map_count = 0;
  spinlock_init(&mm->page_table_lock);
  spinlock_init(&mm->mmap_lock);
  // pml4 should be set by the caller (e.g., fork or init)
}

void mm_destroy(struct mm_struct *mm) {
  if (!mm)
    return;

  spinlock_lock(&mm->mmap_lock);
  struct vm_area_struct *vma = mm->mmap;

  // Iterate and free all VMAs
  while (vma) {
    struct vm_area_struct *next = vma->vm_next;
    // rb_erase not strictly needed if we are destroying the whole tree,
    // but removing from tree keeps state consistent if we crash halfway.
    // For destruction, just freeing the struct is faster/sufficient if no
    // concurrent access.
    vma_free(vma);
    vma = next;
  }

  mm->mmap = NULL;
  mm->mm_rb = RB_ROOT;
  mm->map_count = 0;

  spinlock_unlock(&mm->mmap_lock);

  // Note: We do not free the mm struct itself here, as it might be static
  // (init_mm) or embedded in task_struct. Caller handles container.
}

struct vm_area_struct *vma_create(uint64_t start, uint64_t end,
                                  uint64_t flags) {
  struct vm_area_struct *vma = NULL;

  /* Try kmalloc first if widely available */
  /* Note: We rely on kmalloc returning NULL if not initialized */
  vma = (struct vm_area_struct *)kmalloc(sizeof(struct vm_area_struct));

  if (!vma) {
    // Fallback to bootstrap allocator
    spinlock_lock(&bootstrap_lock);
    for (int i = 0; i < BOOTSTRAP_VMA_COUNT; i++) {
      if (!bootstrap_in_use[i]) {
        bootstrap_in_use[i] = true;
        vma = &bootstrap_vmas[i];
        break;
      }
    }
    spinlock_unlock(&bootstrap_lock);
  }

  if (!vma) {
    // Critical failure if we can't allocate even from bootstrap
    return NULL;
  }

  memset(vma, 0, sizeof(struct vm_area_struct));
  vma->vm_start = start;
  vma->vm_end = end;
  vma->vm_flags = flags;
  vma->vm_rb = (struct rb_node){0};

  return vma;
}

void vma_free(struct vm_area_struct *vma) {
  if (!vma)
    return;

  // Check if it's a bootstrap VMA
  bool is_bootstrap =
      (vma >= bootstrap_vmas && vma < bootstrap_vmas + BOOTSTRAP_VMA_COUNT);

  if (is_bootstrap) {
    // Calculate index and mark as free
    uint64_t index = vma - bootstrap_vmas;
    spinlock_lock(&bootstrap_lock);
    bootstrap_in_use[index] = false;
    spinlock_unlock(&bootstrap_lock);
    return;
  }

  kfree(vma);
}

struct vm_area_struct *vma_find(struct mm_struct *mm, uint64_t addr) {
  struct rb_node *node = mm->mm_rb.rb_node;
  struct vm_area_struct *vma;

  while (node) {
    vma = rb_entry(node, struct vm_area_struct, vm_rb);

    if (addr < vma->vm_start) {
      node = node->rb_left;
    } else if (addr >= vma->vm_end) {
      node = node->rb_right;
    } else {
      return vma; // Match
    }
  }
  return NULL;
}

int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma) {
  struct rb_node **new = &mm->mm_rb.rb_node;
  struct rb_node *parent = NULL;
  struct vm_area_struct *this_vma;

  /* Figure out where to put new node */
  while (*new) {
    this_vma = rb_entry(*new, struct vm_area_struct, vm_rb);
    parent = *new;

    // Check for overlap
    if (vma->vm_end > this_vma->vm_start && vma->vm_start < this_vma->vm_end) {
      // Overlap detected!
      return -1;
    }

    if (vma->vm_start < this_vma->vm_start)
      new = &((*new)->rb_left);
    else if (vma->vm_start >= this_vma->vm_end)
      new = &((*new)->rb_right);
    else
      return -1; // Should be caught by overlap check, but sanity check
  }

  /* Add new node and rebalance tree */
  vma->vm_mm = mm;
  rb_link_node(&vma->vm_rb, parent, new);
  rb_insert_color(&vma->vm_rb, &mm->mm_rb);

  mm->map_count++;

  // Add to linked list (simplified sorted insertion or just add to head)
  // For now, let's just prepend to mmap list for simplicity,
  // real implementations keep it sorted.
  vma->vm_next = mm->mmap;
  vma->vm_prev = NULL;
  if (mm->mmap)
    mm->mmap->vm_prev = vma;
  mm->mmap = vma;

  return 0;
}

void vma_dump(struct mm_struct *mm) {
  struct rb_node *node;
  struct vm_area_struct *vma;

  printk(VMA_CLASS "VMA dump for mm: %p\n", mm);
  // Iterate in-order
  for (node = rb_first(&mm->mm_rb); node; node = rb_next(node)) {
    vma = rb_entry(node, struct vm_area_struct, vm_rb);
    printk(VMA_CLASS "  [%llx - %llx] flags: %llx\n", vma->vm_start,
           vma->vm_end, vma->vm_flags);
  }
}

/* Find a free region of given size between start and end address */
uint64_t vma_find_free_region(struct mm_struct *mm, size_t size,
                              uint64_t range_start, uint64_t range_end) {
  if (!mm || size == 0)
    return 0;

  /* Align size to page boundary */
  size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

  uint64_t current_addr = range_start;
  struct vm_area_struct *vma = mm->mmap; // Sorted linked list of VMAs

  /* Fast forward to the start of our search range */
  while (vma && vma->vm_end <= range_start) {
    vma = vma->vm_next;
  }

  while (vma) {
    // Check gap between current_addr and vma->vm_start
    if (vma->vm_start > current_addr) {
      uint64_t gap_size = vma->vm_start - current_addr;
      if (gap_size >= size) {
        // Found a hole! Check if it fits within range_end
        if (current_addr + size <= range_end) {
          return current_addr;
        } else {
          return 0; // Exceeded range
        }
      }
    }

    // Move current_addr to end of this VMA
    current_addr = vma->vm_end;

    // Optimize: Align current_addr to page boundary just in case (VMAs should
    // be aligned though)
    current_addr = (current_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    if (current_addr >= range_end)
      return 0;

    vma = vma->vm_next;
  }

  // Check gap after last VMA
  if (range_end > current_addr) {
    if (range_end - current_addr >= size) {
      return current_addr;
    }
  }

  return 0; // No space found
}

/* Global kernel mm_struct */
struct mm_struct init_mm;

void vma_test(void) {

  struct mm_struct test_mm;
  mm_init(&test_mm);

  // Test 1: Insert non-overlapping
  struct vm_area_struct *v1 = vma_create(0x1000, 0x2000, VM_READ);
  if (!v1 || vma_insert(&test_mm, v1) != 0)
    panic(VMA_CLASS "VMA Test 1 Failed: Insert v1");

  struct vm_area_struct *v2 = vma_create(0x3000, 0x4000, VM_READ);
  if (!v2 || vma_insert(&test_mm, v2) != 0)
    panic(VMA_CLASS "VMA Test 1 Failed: Insert v2");

  // Test 2: Find
  if (vma_find(&test_mm, 0x1100) != v1)
    panic(VMA_CLASS "VMA Test 2 Failed: Find v1");
  if (vma_find(&test_mm, 0x3500) != v2)
    panic(VMA_CLASS "VMA Test 2 Failed: Find v2");
  if (vma_find(&test_mm, 0x2500) != NULL)
    panic(VMA_CLASS "VMA Test 2 Failed: Find gap");

  // Test 3: Overlap
  struct vm_area_struct *v3 = vma_create(0x1500, 0x2500, VM_READ);
  if (!v3 || vma_insert(&test_mm, v3) == 0)
    panic(VMA_CLASS "VMA Test 3 Failed: Overlap allowed");
  vma_free(v3); // Succeeded alloc, failed insert

  // Cleanup using proper destroy function
  mm_destroy(&test_mm);
}
