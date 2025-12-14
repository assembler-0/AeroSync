#pragma once

#include <compiler.h>
#include <kernel/spinlock.h>
#include <kernel/types.h>
#include <linux/list.h>
#include <linux/rbtree.h>

/* VMA Flags */
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008

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

  /* Linked list of VMAs sorted by address */
  struct list_head vm_list;

  uint64_t vm_flags; /* Flags as listed above */

  /* TODO: Backing store/file pointers will go here */
};

/*
 * mm_struct
 * Represents the entire address space of a task.
 */
struct mm_struct {
  struct rb_root mm_rb;        /* Root of the VMA Red-Black Tree */
  struct list_head mmap_list; /* List of VMAs */

  uint64_t *pml4; /* Physical address of the top-level page table */

  spinlock_t page_table_lock; /* Protects page table modifications */
  spinlock_t mmap_lock;       /* Protects VMA list/tree modifications */

  int map_count; /* Number of VMAs */

  uint64_t start_code, end_code, start_data, end_data;
  uint64_t start_brk, brk, start_stack;
};
