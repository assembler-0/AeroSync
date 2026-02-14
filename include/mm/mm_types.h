#pragma once

#include <compiler.h>
#include <aerosync/spinlock.h>
#include <aerosync/types.h>
#include <linux/list.h>
#include <aerosync/atomic.h>

#include <linux/rcupdate.h>
#include <aerosync/rw_semaphore.h>
#include <linux/maple_tree.h>

struct vm_area_struct;
struct vm_fault;
struct vm_object;

/* VMA Flags */
#define VM_READ 0x00000001
#define VM_WRITE 0x00000002
#define VM_EXEC 0x00000004
#define VM_SHARED 0x00000008

/* Permissions and State */
#define VM_MAYREAD 0x00000010
#define VM_MAYWRITE 0x00000020
#define VM_MAYEXEC 0x00000040
#define VM_MAYSHARE 0x00000080

#define VM_GROWSDOWN 0x00000100
#define VM_GROWSUP 0x00000200
#define VM_PFNMAP 0x00000400
#define VM_DENYWRITE 0x00000800
#define VM_VMALLOC   0x00001000

#define VM_LOCKED 0x00002000
#define VM_IO 0x00004000
#define VM_DONTCOPY 0x00008000
#define VM_DONTEXPAND 0x00010000
#define VM_RESERVED 0x00020000
#define VM_ACCOUNT 0x00040000
#define VM_NORESERVE 0x00080000
#define VM_HUGETLB 0x00100000
#define VM_SYNC 0x00200000
#define VM_USER 0x00400000
#define VM_STACK 0x00800000
#define VM_HUGE 0x01000000
#define VM_ALLOC_LAZY 0x02000000

#define VM_RANDOM     0x04000000
#define VM_SEQUENTIAL 0x08000000
#define VM_HUGEPAGE   0x10000000
#define VM_NOHUGEPAGE 0x20000000

#define VMA_MAGIC 0x564d415f41524541ULL /* "VMA_AREA" */

/* Additional state flags */
#define VM_VMALLOC_PCP 0x100000000ULL
#define VM_LAZY_FREE   0x200000000ULL

/* Cache Policy Flags */
#define VM_CACHE_WB 0x00000000
#define VM_CACHE_WT 0x10000000
#define VM_CACHE_UC 0x20000000
#define VM_CACHE_WC 0x30000000
#define VM_CACHE_WP 0x40000000
#define VM_CACHE_MASK 0xF0000000

/*
 * anon_vma_chain
 * Links a VMA to an anon_vma. Essential for complex fork/COW hierarchies.
 */
struct anon_vma_chain {
  struct vm_area_struct *vma;
  struct anon_vma *anon_vma;
  struct list_head same_vma;     /* Node in vma->anon_vma_chain */
  struct list_head same_anon_vma; /* Node in anon_vma->head */
  struct list_head unmap_list;   /* Temporary list for RMAP operations */
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
  int (*page_mkwrite)(struct vm_area_struct *vma, struct vm_fault *vmf);
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
  struct folio *folio; /* The folio containing the page (Primary) */
  uint64_t prot;     /* Protection flags for this specific mapping */
};

/*
 * vm_area_struct
 * Represents a contiguous range of virtual memory with consistent permissions.
 * Managed effectively by a Maple Tree.
 */
alignas(sizeof(long)) struct vm_area_struct {
  uint64_t vma_magic;      /* Integrity check */
  struct mm_struct *vm_mm; /* The address space we belong to */

  uint64_t vm_start; /* Our start address within vm_mm */
  uint64_t vm_end;   /* The first byte after our end address within vm_mm */

  uint64_t vm_flags; /* Flags as listed above */
  uint64_t vm_page_prot; /* Prot flags for the page table */
  uint32_t vma_seq; /* Sequence counter for speculative faults */
  int preferred_node; /* Preferred NUMA node for this VMA (-1 for none) */

  /* Operations for this VMA */
  const struct vm_operations_struct *vm_ops;
  uint64_t vm_pgoff; /* Offset (within vm_file) in PAGE_SIZE units */

  /* Chained RMAP (Anonymous) */
  struct anon_vma *anon_vma;      /* Primary/Root anon_vma */
  struct list_head anon_vma_chain; /* List of struct anon_vma_chain */

  /* Object Mapping (File/Shared) */
  struct vm_object *vm_obj;
  struct list_head vm_shared; /* Node in obj->i_mmap */

  struct rcu_head rcu;
  struct rw_semaphore vm_lock;

  void *vm_private_data;
};

#include <aerosync/atomic.h>
#include <aerosync/sched/cpumask.h>

/*
 * mm_struct
 * Represents the entire address space of a task.
 */
struct mm_struct {
  struct maple_tree mm_mt;    /* Maple Tree for VMA management */
  uint64_t vmacache_seqnum;   /* Per-thread VMA cache invalidation sequence */

  uint64_t *pml_root; /* Physical address of the top-level page table */

  struct rw_semaphore mmap_lock; /* Protects VMA list/tree modifications */

  atomic_t mm_count; /* Reference count */
  int map_count; /* Number of VMAs */

  /* Speculative Page Fault Tracking */
  atomic_t mmap_seq;

  uint64_t mmap_base; /* Hint for where to start looking for free space */
  uint64_t last_hole; /* Cache last successful hole for O(1) Sequential Alloc */

  /* Accounting */
  size_t total_vm;  /* Total number of pages mapped */
  atomic64_t rss;   /* Resident Set Size (actually in memory) */
  size_t locked_vm; /* Number of locked pages */
  size_t pinned_vm; /* Refcount permanently increased */
  size_t data_vm;   /* VM_WRITE & ~VM_SHARED & ~VM_STACK */
  size_t exec_vm;   /* VM_EXEC & ~VM_WRITE */
  size_t stack_vm;  /* VM_STACK */

  uint64_t start_code, end_code, start_data, end_data;
  uint64_t start_brk, brk, start_stack;
  uint64_t arg_start, arg_end, env_start, env_end;

  int preferred_node; /* Default NUMA node for this address space */

  struct cpumask cpu_mask; /* CPUs currently using this mm */
  struct resdomain *rd;    /* Resource domain for this address space */
};
