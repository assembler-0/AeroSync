#pragma once

#include <compiler.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <kernel/atomic.h>

struct vm_area_struct;
struct vm_fault;
struct vm_object;

/* VMA Flags */
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008

/*
 * anon_vma_chain
 * Links a VMA to an anon_vma. Essential for complex fork/COW hierarchies.
 */
struct anon_vma_chain {
  struct vm_area_struct *vma;
  struct anon_vma *anon_vma;
  struct list_head same_vma;     /* Node in vma->anon_vma_chain */
  struct list_head same_anon_vma; /* Node in anon_vma->head */
};

/*
 * anon_vma
 * Container for VMAs that share anonymous pages.
 */
struct anon_vma {
  spinlock_t lock;
  struct list_head head; /* List of anon_vma_chain */
  struct anon_vma *parent;
  atomic_t refcount;
};

/*
 * vm_operations_struct
 * Set of functions to handle VMA events like faults, opening, and closing.
 */
struct vm_operations_struct {
  void (*open)(struct vm_area_struct *area);
  void (*close)(struct vm_area_struct *area);
  int (*fault)(struct vm_area_struct *vma, struct vm_fault *vmf);
  /* TODO: page_mkwrite for COW on shared mappings */
};

/*
 * vm_fault
 * Context for a page fault handler.
 */
struct vm_fault {
  uint64_t address; /* Faulting virtual address */
  unsigned int flags; /* FAULT_FLAG_xxx */
  uint64_t pgoff; /* Page offset within the object */

  /* Output from the fault handler */
  struct page *page; /* The physical page to be mapped */
};

/*
 * vm_area_struct
 * Represents a contiguous range of virtual memory with consistent permissions.
 * Managed effectively by a Red-Black Tree.
 */
struct __aligned(sizeof(long)) vm_area_struct {
  struct rb_node vm_rb; /* Tree node for mm->mm_rb */

  struct mm_struct *vm_mm; /* The address space we belong to */

  uint64_t vm_start; /* Our start address within vm_mm */
  uint64_t vm_end;   /* The first byte after our end address within vm_mm */

  /* For Augmented RB-Tree gap tracking */
  uint64_t vm_rb_max_gap;

  /* Linked list of VMAs sorted by address */
  struct list_head vm_list;

  uint64_t vm_flags; /* Flags as listed above */

  /* Operations for this VMA */
  const struct vm_operations_struct *vm_ops;
  uint64_t vm_pgoff; /* Offset (within vm_file) in PAGE_SIZE units */

  /* Chained RMAP (Anonymous) */
  struct anon_vma *anon_vma;      /* Primary/Root anon_vma */
  struct list_head anon_vma_chain; /* List of struct anon_vma_chain */

  /* Object Mapping (File/Shared) */
  struct vm_object *vm_obj;
  struct list_head vm_shared; /* Node in obj->i_mmap */

  /* TODO: Backing store/file pointers will go here */
  void *vm_private_data;
};

#include <kernel/atomic.h>
#include <kernel/rw_semaphore.h>
#include <kernel/sched/cpumask.h>

/*
 * mm_struct
 * Represents the entire address space of a task.
 */
struct mm_struct {
  struct rb_root mm_rb;        /* Root of the VMA Red-Black Tree */
  uint64_t vmacache_seqnum;   /* Per-thread VMA cache invalidation sequence */
  struct list_head mmap_list; /* List of VMAs */

  uint64_t *pml4; /* Physical address of the top-level page table */

  spinlock_t page_table_lock; /* Protects page table modifications (fallback) */
  struct rw_semaphore mmap_lock; /* Protects VMA list/tree modifications */

  atomic_t mm_count; /* Reference count */
  int map_count; /* Number of VMAs */

  uint64_t mmap_base; /* Hint for where to start looking for free space */

  uint64_t start_code, end_code, start_data, end_data;
  uint64_t start_brk, brk, start_stack;

  struct cpumask cpu_mask; /* CPUs currently using this mm */
};
