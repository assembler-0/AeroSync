#include <arch/x64/mm/pmm.h>
#include <kernel/classes.h>
#include <kernel/panic.h>
#include <lib/printk.h>
#include <linux/rbtree.h>
#include <linux/list.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/slab.h>
#include <mm/vma.h>

#define BOOTSTRAP_VMA_COUNT 128
static struct vm_area_struct bootstrap_vmas[BOOTSTRAP_VMA_COUNT];
static bool
    bootstrap_in_use[BOOTSTRAP_VMA_COUNT]; // all false (0) by default in BSS
static spinlock_t bootstrap_lock = 0;

void mm_init(struct mm_struct *mm) {
  mm->mm_rb = RB_ROOT;
  INIT_LIST_HEAD(&mm->mmap_list);
  mm->map_count = 0;
  spinlock_init(&mm->page_table_lock);
  spinlock_init(&mm->mmap_lock);
  // pml4 should be set by the caller (e.g., fork or init)
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
  INIT_LIST_HEAD(&vma->vm_list);

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

/*
 * Helper: Insert VMA into the sorted linked list
 */
static void __vma_link_list(struct mm_struct *mm, struct vm_area_struct *vma) {
    struct vm_area_struct *curr;

    // Iterate to find the insertion point
    list_for_each_entry(curr, &mm->mmap_list, vm_list) {
        if (curr->vm_start > vma->vm_start) {
            // Insert before curr
            list_add_tail(&vma->vm_list, &curr->vm_list);
            return;
        }
    }

    // If list is empty or vma is the last element
    list_add_tail(&vma->vm_list, &mm->mmap_list);
}

/*
 * Helper: Unlink VMA from the sorted linked list
 */
static void __vma_unlink_list(struct mm_struct *mm, struct vm_area_struct *vma) {
    (void)mm; // unused
    list_del(&vma->vm_list);
}

int vma_insert(struct mm_struct *mm, struct vm_area_struct *vma) {
  struct rb_node **new = &mm->mm_rb.rb_node;
  struct rb_node *parent = NULL;
  struct vm_area_struct *this_vma;

  /* 1. Insert into RB-Tree (O(log N)) */
  while (*new) {
    this_vma = rb_entry(*new, struct vm_area_struct, vm_rb);
    parent = *new;

    // Check for overlap
    if (vma->vm_end > this_vma->vm_start && vma->vm_start < this_vma->vm_end) {
      return -1; // Overlap
    }

    if (vma->vm_start < this_vma->vm_start)
      new = &((*new)->rb_left);
    else if (vma->vm_start >= this_vma->vm_end)
      new = &((*new)->rb_right);
    else
      return -1; // Should be impossible due to overlap check
  }

  vma->vm_mm = mm;
  rb_link_node(&vma->vm_rb, parent, new);
  rb_insert_color(&vma->vm_rb, &mm->mm_rb);

  /* 2. Insert into Sorted Linked List (O(N)) */
  __vma_link_list(mm, vma);

  mm->map_count++;
  return 0;
}

/*
 * Remove a VMA from the mm_struct (RB-Tree and List), but do NOT free it.
 * Caller is responsible for freeing.
 */
void vma_remove(struct mm_struct *mm, struct vm_area_struct *vma) {
    if (!mm || !vma) return;

    // 1. Remove from RB-Tree
    rb_erase(&vma->vm_rb, &mm->mm_rb);

    // 2. Remove from Linked List
    __vma_unlink_list(mm, vma);

    mm->map_count--;
    vma->vm_mm = NULL;
}

/*
 * Destroys all VMAs in the MM.
 */
void mm_destroy(struct mm_struct *mm) {
  if (!mm) return;

  spinlock_lock(&mm->mmap_lock);

  struct vm_area_struct *vma, *tmp;

  list_for_each_entry_safe(vma, tmp, &mm->mmap_list, vm_list) {
    // Optional: Paranoia check
    if (vma->vm_mm != mm) {
        printk(KERN_WARNING "VMA %p does not belong to MM %p!\n", vma, mm);
    }
    vma_free(vma);
  }

  INIT_LIST_HEAD(&mm->mmap_list);
  mm->mm_rb = RB_ROOT;
  mm->map_count = 0;

  spinlock_unlock(&mm->mmap_lock);
}

void vma_dump(struct mm_struct *mm) {
  struct vm_area_struct *vma;

  printk(VMA_CLASS "VMA dump for mm: %p\n", mm);
  // Iterate list
  list_for_each_entry(vma, &mm->mmap_list, vm_list) {
    printk(VMA_CLASS "  [%llx - %llx] flags: %llx\n", vma->vm_start,
           vma->vm_end,
           vma->vm_flags);
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
  struct vm_area_struct *vma;
  
  // Optimistic check: if list is empty
  if (list_empty(&mm->mmap_list)) {
      if (range_end > range_start && range_end - range_start >= size)
          return range_start;
      return 0;
  }

  list_for_each_entry(vma, &mm->mmap_list, vm_list) {
      // If VMA ends before our range, just move on (but update current_addr if needed? 
      // Actually if vma->vm_end < range_start, it's irrelevant, but current_addr is range_start)
      if (vma->vm_end <= range_start) continue;

      // Now vma->vm_end > range_start. 
      // If vma->vm_start is also < range_start, then this VMA covers the start.
      // current_addr must be moved to vma->vm_end.
      
      // Check gap between current_addr and vma->vm_start
      if (vma->vm_start > current_addr) {
          uint64_t gap = vma->vm_start - current_addr;
          if (gap >= size) {
              if (current_addr + size <= range_end) return current_addr;
              return 0; // Exceeded max range
          }
      }

      current_addr = vma->vm_end;
      // Align
      current_addr = (current_addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

      if (current_addr >= range_end) return 0;
  }

  // Check tail gap
  if (range_end > current_addr) {
      if (range_end - current_addr >= size) return current_addr;
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