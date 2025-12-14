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
static int bootstrap_vma_used = 0;
static spinlock_t bootstrap_lock = 0;

void mm_init(struct mm_struct *mm) {
  mm->mm_rb = RB_ROOT;
  mm->mmap = NULL;
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
    if (bootstrap_vma_used < BOOTSTRAP_VMA_COUNT) {
      vma = &bootstrap_vmas[bootstrap_vma_used++];
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
    // We generally don't free bootstrap VMAs as they are compact
    // and used for the kernel core. Leaking them is 'fine' for now
    // or we mark them as free in a bitmap if we wanted to be fancy.
    // For now, no-op.
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
    printk(VMA_CLASS "  [%llx - %llx] flags: %llx\n", vma->vm_start, vma->vm_end,
           vma->vm_flags);
  }
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
}
