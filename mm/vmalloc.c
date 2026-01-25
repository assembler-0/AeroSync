/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vmalloc.c
 * @brief Ultra-High Performance Hybrid Kernel Virtual Memory Allocator
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/atomic.h>
#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/sched/process.h>
#include <aerosync/sched/sched.h>
#include <aerosync/spinlock.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/mm/paging.h>
#include <arch/x86_64/mm/tlb.h>
#include <arch/x86_64/mm/vmm.h>
#include <arch/x86_64/percpu.h>
#include <lib/bitmap.h>
#include <lib/math.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <linux/rbtree_augmented.h>
#include <linux/rculist.h>
#include <linux/rcupdate.h>
#include <mm/mm_types.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/vm_object.h>
#include <mm/vma.h>
#include <mm/vmalloc.h>
#include <mm/zone.h>

/* ========================================================================
 * Configuration & Constants
 * ======================================================================= */

#define VM_LAZY_FREE_THRESHOLD (32 * 1024 * 1024ULL)
#define PCP_VA_THRESHOLD 128
#define PCP_RANGE_THRESHOLD 16

#define VMAP_BLOCK_PAGES VMAP_BBMAP_BITS
#define VMAP_BLOCK_SIZE_BYTES (VMAP_BLOCK_PAGES << PAGE_SHIFT)

#define PCP_BIN_1P 0
#define PCP_BIN_2P 1
#define PCP_BIN_4P 2
#define PCP_BIN_8P 3
#define PCP_BINS 4

/* ========================================================================
 * Data Structures
 * ======================================================================= */

struct vmap_node {
  spinlock_t lock;
  struct rb_root root;
  struct list_head list;
  struct list_head purge_list;
  atomic_long_t nr_purged;
  int nid;
} __aligned(64);

struct vmap_pcp {
  spinlock_t lock;
  struct list_head free_va;
  int nr_va;
  struct list_head bins[PCP_BINS];
  int bin_count[PCP_BINS];
} __aligned(64);

/* ========================================================================
 * Global State
 * ======================================================================= */

static struct vmap_node vmap_nodes[MAX_NUMNODES];
static DEFINE_PER_CPU(struct vmap_pcp, vmap_pcp);
static struct kmem_cache *vmap_area_cachep;
static struct kmem_cache *vmap_block_cachep;

struct vmap_block_queue {
  spinlock_t lock;
  struct list_head free;
};

static DEFINE_PER_CPU(struct vmap_block_queue, vmap_block_queues);

/* ========================================================================
 * Augmented RB-Tree (Gap Tracking)
 * ======================================================================= */

static inline unsigned long vmap_area_compute_gap(struct vmap_area *va) {
  struct rb_node *prev_node = rb_prev(&va->rb_node);
  if (prev_node) {
    struct vmap_area *prev = rb_entry(prev_node, struct vmap_area, rb_node);
    return va->va_start - prev->va_end;
  }
  return va->va_start - VMALLOC_VIRT_BASE;
}

static unsigned long vmap_area_rb_compute_max_gap(struct vmap_area *va) {
  unsigned long max_gap = vmap_area_compute_gap(va);
  unsigned long cur;

  if (va->rb_node.rb_left) {
    cur = rb_entry(va->rb_node.rb_left, struct vmap_area, rb_node)->rb_max_gap;
    if (cur > max_gap)
      max_gap = cur;
  }
  if (va->rb_node.rb_right) {
    cur = rb_entry(va->rb_node.rb_right, struct vmap_area, rb_node)->rb_max_gap;
    if (cur > max_gap)
      max_gap = cur;
  }
  return max_gap;
}

RB_DECLARE_CALLBACKS_MAX(static, vmap_area_rb_callbacks, struct vmap_area,
                         rb_node, unsigned long, rb_max_gap,
                         vmap_area_rb_compute_max_gap)

/* ========================================================================
 * Internal Metadata Helpers
 * ======================================================================= */

static struct vmap_area *alloc_vmap_area_metadata(int nid) {
  struct vmap_pcp *pcp = this_cpu_ptr(vmap_pcp);
  struct vmap_area *va = NULL;

  irq_flags_t flags = spinlock_lock_irqsave(&pcp->lock);
  if (!list_empty(&pcp->free_va)) {
    va = list_first_entry(&pcp->free_va, struct vmap_area, list);
    list_del(&va->list);
    pcp->nr_va--;
    spinlock_unlock_irqrestore(&pcp->lock, flags);
    return va;
  }
  spinlock_unlock_irqrestore(&pcp->lock, flags);

  va = kmem_cache_alloc_node(vmap_area_cachep, nid);
  if (va)
    RB_CLEAR_NODE(&va->rb_node);
  return va;
}

static void free_vmap_area_metadata(struct vmap_area *va) {
  struct vmap_pcp *pcp = this_cpu_ptr(vmap_pcp);

  va->flags = 0;
  RB_CLEAR_NODE(&va->rb_node);

  irq_flags_t flags = spinlock_lock_irqsave(&pcp->lock);
  if (pcp->nr_va < PCP_VA_THRESHOLD) {
    list_add(&va->list, &pcp->free_va);
    pcp->nr_va++;
    spinlock_unlock_irqrestore(&pcp->lock, flags);
    return;
  }
  spinlock_unlock_irqrestore(&pcp->lock, flags);

  kmem_cache_free(vmap_area_cachep, va);
}

static inline int pcp_bin_index(unsigned long pages) {
  if (pages == 1)
    return PCP_BIN_1P;
  if (pages == 2)
    return PCP_BIN_2P;
  if (pages == 4)
    return PCP_BIN_4P;
  if (pages == 8)
    return PCP_BIN_8P;
  return -1;
}

/* ========================================================================
 * Virtual Address Space Management
 * ======================================================================= */

static struct vmap_area *find_vmap_area(unsigned long addr) {
  struct rb_node *n;
  struct vmap_area *va;

  rcu_read_lock();
  for (int i = 0; i < MAX_NUMNODES; i++) {
    n = rcu_dereference(vmap_nodes[i].root.rb_node);
    while (n) {
      va = rb_entry(n, struct vmap_area, rb_node);
      if (addr < va->va_start)
        n = rcu_dereference(n->rb_left);
      else if (addr >= va->va_end)
        n = rcu_dereference(n->rb_right);
      else {
        rcu_read_unlock();
        return va;
      }
    }
  }
  rcu_read_unlock();
  return NULL;
}

static struct vmap_area *alloc_vmap_area(unsigned long size,
                                         unsigned long align,
                                         unsigned long vstart,
                                         unsigned long vend, int nid) {
  struct vmap_area *va, *tmp;
  struct rb_node **p, *parent = NULL;
  unsigned long addr = 0;
  int target_node = (nid == NUMA_NO_NODE) ? this_node() : nid;

  size = PAGE_ALIGN_UP(size);
  align = max(align, PAGE_SIZE);

  /*
   * Add a guard page at the end of the requested range.
   * This is unmapped and acts as a safety buffer for kernel overflows.
   */
  unsigned long real_size = size + PAGE_SIZE;

  va = alloc_vmap_area_metadata(target_node);
  if (!va)
    return NULL;

  /*
   * CRITICAL BUG FIX: We must ensure the allocated range doesn't overlap
   * with ANY VMA on ANY node. Since we have per-node RB-trees, we must
   * coordinate. For simplicity and correctness, we find a gap that is
   * free across ALL nodes.
   */
  
  // Start searching from vstart
  addr = ALIGN_UP(vstart, align);

  while (addr + real_size <= vend) {
    bool conflict = false;
    unsigned long next_addr = 0;

    for (int i = 0; i < MAX_NUMNODES; i++) {
      struct vmap_node *vn = &vmap_nodes[i];
      irq_flags_t flags = spinlock_lock_irqsave(&vn->lock);
      
      struct rb_node *n = vn->root.rb_node;
      while (n) {
        tmp = rb_entry(n, struct vmap_area, rb_node);
        if (addr < tmp->va_end && (addr + real_size) > tmp->va_start) {
          // Overlap!
          conflict = true;
          next_addr = max(next_addr, tmp->va_end);
          break;
        }
        if (addr < tmp->va_start)
          n = n->rb_left;
        else
          n = n->rb_right;
      }
      spinlock_unlock_irqrestore(&vn->lock, flags);
      if (conflict) break;
    }

    if (!conflict) {
      // Found a gap!
      goto found;
    }

    addr = ALIGN_UP(next_addr, align);
  }

  free_vmap_area_metadata(va);
  return NULL;

found:
  va->va_start = addr;
  va->va_end = addr + real_size;
  va->nid = target_node;
  va->flags = VMAP_AREA_USED;
  va->vb = NULL;

  struct vmap_node *target_vn = &vmap_nodes[target_node];
  irq_flags_t flags = spinlock_lock_irqsave(&target_vn->lock);

  p = &target_vn->root.rb_node;
  while (*p) {
    parent = *p;
    tmp = rb_entry(parent, struct vmap_area, rb_node);
    if (va->va_start < tmp->va_start)
      p = &(*p)->rb_left;
    else
      p = &(*p)->rb_right;
  }
  rb_link_node(&va->rb_node, parent, p);
  rb_insert_augmented(&va->rb_node, &target_vn->root, &vmap_area_rb_callbacks);
  list_add_tail_rcu(&va->list, &target_vn->list);

  spinlock_unlock_irqrestore(&target_vn->lock, flags);
  return va;
}

/* ========================================================================
 * Purging & TLB Invalidation
 * ======================================================================= */

static void __purge_vmap_node(struct vmap_node *vn) {
  struct vmap_area *va, *n;
  LIST_HEAD(local_purge_list);

  irq_flags_t flags = spinlock_lock_irqsave(&vn->lock);
  list_splice_init(&vn->purge_list, &local_purge_list);
  atomic_long_set(&vn->nr_purged, 0);
  spinlock_unlock_irqrestore(&vn->lock, flags);

  if (list_empty(&local_purge_list))
    return;

  list_for_each_entry(va, &local_purge_list, purge_list) {
    /*
     * va_end includes the guard page.
     * We only need to unmap the data pages.
     */
    size_t data_pages = (va->va_end - va->va_start - PAGE_SIZE) >> PAGE_SHIFT;
    vmm_unmap_pages(&init_mm, va->va_start, data_pages);
  }

  /*
   * OPTIMIZATION: Instead of flushing the entire VMALLOC range,
   * only flush the specific ranges being purged to reduce overhead.
   * This is critical for uACPI hardware access timing.
   */
  list_for_each_entry(va, &local_purge_list, purge_list) {
    vmm_tlb_shootdown(&init_mm, va->va_start, va->va_end);
  }

  list_for_each_entry_safe(va, n, &local_purge_list, purge_list) {
    list_del(&va->purge_list);

    /*
     * Now that the TLB is flushed, we can safely remove the area from the
     * allocator's view. We need to re-acquire the lock for rb_erase.
     */
    spinlock_lock(&vn->lock);
    rb_erase_augmented(&va->rb_node, &vn->root, &vmap_area_rb_callbacks);
    list_del_rcu(&va->list);
    RB_CLEAR_NODE(&va->rb_node);
    spinlock_unlock(&vn->lock);

    free_vmap_area_metadata(va);
  }
}

static int kvmap_purged_thread(void *data) {
  (void)data;
  while (1) {
    for (int i = 0; i < MAX_NUMNODES; i++) {
      if (atomic_long_read(&vmap_nodes[i].nr_purged) >
          (VM_LAZY_FREE_THRESHOLD >> PAGE_SHIFT)) {
        __purge_vmap_node(&vmap_nodes[i]);
      }
    }
    schedule();
  }
  return 0;
}

/* ========================================================================
 * vmap_block
 * ======================================================================= */

static struct vmap_block *new_vmap_block(int nid) {
  struct vmap_block *vb;
  struct vmap_area *va;

  int target_node = (nid == NUMA_NO_NODE) ? this_node() : nid;

  vb = kmem_cache_alloc_node(vmap_block_cachep, target_node);
  if (!vb)
    return NULL;

  va = alloc_vmap_area(VMAP_BLOCK_SIZE_BYTES, VMAP_BLOCK_SIZE_BYTES,
                       VMALLOC_VIRT_BASE, VMALLOC_VIRT_END, target_node);
  if (!va) {
    kmem_cache_free(vmap_block_cachep, vb);
    return NULL;
  }

  va->flags |= VMAP_AREA_BLOCK;
  va->vb = vb;
  spinlock_init(&vb->lock);
  vb->va = va;
  vb->free_map = 0;
  vb->dirty_map = 0;
  memset(vb->sizes, 0, sizeof(vb->sizes));
  vb->nid = va->nid;
  vb->cpu = smp_get_id();

  struct vmap_block_queue *vbq = this_cpu_ptr(vmap_block_queues);
  irq_flags_t flags = spinlock_lock_irqsave(&vbq->lock);
  list_add_rcu(&vb->list, &vbq->free);
  spinlock_unlock_irqrestore(&vbq->lock, flags);

  return vb;
}

static void *vb_alloc(size_t size, int nid) {
  struct vmap_block_queue *vbq = this_cpu_ptr(vmap_block_queues);
  struct vmap_block *vb;
  unsigned long pages = size >> PAGE_SHIFT;

  rcu_read_lock();
  list_for_each_entry_rcu(vb, &vbq->free, list) {
    if (vb->nid != nid && nid != NUMA_NO_NODE)
      continue;
    if (spinlock_trylock(&vb->lock)) {
      unsigned long bit = bitmap_find_next_zero_area(
          &vb->free_map, VMAP_BBMAP_BITS, 0, pages, 0);
      if (bit < VMAP_BBMAP_BITS) {
        bitmap_set(&vb->free_map, bit, pages);
        vb->sizes[bit] = (uint8_t)pages;
        spinlock_unlock(&vb->lock);
        rcu_read_unlock();
        return (void *)(vb->va->va_start + (bit << PAGE_SHIFT));
      }
      spinlock_unlock(&vb->lock);
    }
  }
  rcu_read_unlock();

  vb = new_vmap_block(nid);
  if (!vb)
    return NULL;
  spinlock_lock(&vb->lock);
  bitmap_set(&vb->free_map, 0, pages);
  vb->sizes[0] = (uint8_t)pages;
  spinlock_unlock(&vb->lock);
  return (void *)vb->va->va_start;
}

/* ========================================================================
 * Core vmalloc Implementation
 * ======================================================================= */

void *vmalloc_node_prot(size_t size, int nid, uint64_t pgprot) {
  struct vmap_area *va = NULL;
  unsigned long addr;
  size_t nr_pages;
  unsigned long cur_vaddr;
  size_t remaining_pages;

  size = PAGE_ALIGN_UP(size);
  if (!size)
    return NULL;
  nr_pages = size >> PAGE_SHIFT;

  if (nid == NUMA_NO_NODE)
    nid = this_node();

  if (size <= (VMAP_BLOCK_SIZE_BYTES / 2)) {
    void *p = vb_alloc(size, nid);
    if (p) {
      addr = (unsigned long)p;
      goto map;
    }
  }

  unsigned long align =
      (size >= VMM_PAGE_SIZE_2M) ? VMM_PAGE_SIZE_2M : PAGE_SIZE;
  va = alloc_vmap_area(size, align, VMALLOC_VIRT_BASE, VMALLOC_VIRT_END, nid);
  if (!va)
    return NULL;
  addr = va->va_start;

map:
  cur_vaddr = addr;
  remaining_pages = nr_pages;

  while (remaining_pages > 0) {
    if (remaining_pages >= 512 && (cur_vaddr & (VMM_PAGE_SIZE_2M - 1)) == 0) {
      struct folio *folio = alloc_pages_node(nid, GFP_KERNEL, 9);
      if (folio) {
        vmm_map_huge_page_no_flush(&init_mm, cur_vaddr, folio_to_phys(folio),
                                   pgprot, VMM_PAGE_SIZE_2M);
        cur_vaddr += VMM_PAGE_SIZE_2M;
        remaining_pages -= 512;
        continue;
      }
    }
    struct folio *folio = alloc_pages_node(nid, GFP_KERNEL, 0);
    if (!folio) {
      vfree((void *)addr);
      return NULL;
    }
    vmm_map_page_no_flush(&init_mm, cur_vaddr, folio_to_phys(folio), pgprot);
    cur_vaddr += PAGE_SIZE;
    remaining_pages--;
  }
  vmm_tlb_shootdown(&init_mm, addr, addr + size);
  return (void *)addr;
}

EXPORT_SYMBOL(vmalloc_node_prot);

void vfree(void *addr) {
  if (!addr)
    return;

  unsigned long vaddr = (unsigned long)addr;

  struct vmap_area *va = find_vmap_area(vaddr);
  if (!va)
    return;

  if (va->flags & VMAP_AREA_BLOCK) {
    struct vmap_block *vb = va->vb;
    unsigned int bit = (vaddr - va->va_start) >> PAGE_SHIFT;
    spinlock_lock(&vb->lock);
    unsigned int pages = vb->sizes[bit];
    if (pages) {
      bitmap_clear(&vb->free_map, bit, pages);
      vb->sizes[bit] = 0;
      vmm_unmap_pages(&init_mm, vaddr, pages);
    }
    spinlock_unlock(&vb->lock);
    return;
  }

  /*
   * OPTIMIZATION: Static mappings (like hardware registers) should be freed
   * immediately without going through the lazy mechanism to avoid unnecessary
   * TLB shootdown overhead for uACPI.
   */
  if (va->flags & VMAP_AREA_STATIC) {
    size_t pages = (va->va_end - va->va_start - PAGE_SIZE) >> PAGE_SHIFT;
    vmm_unmap_pages(&init_mm, va->va_start, pages);
    vmm_tlb_shootdown(&init_mm, va->va_start, va->va_end);

    struct vmap_node *vn = &vmap_nodes[va->nid];
    irq_flags_t flags = spinlock_lock_irqsave(&vn->lock);
    rb_erase_augmented(&va->rb_node, &vn->root, &vmap_area_rb_callbacks);
    list_del_rcu(&va->list);
    RB_CLEAR_NODE(&va->rb_node);
    spinlock_unlock_irqrestore(&vn->lock, flags);

    free_vmap_area_metadata(va);
    return;
  }

  int bin =
      pcp_bin_index((va->va_end - va->va_start - PAGE_SIZE) >> PAGE_SHIFT);
  if (bin >= 0) {
    struct vmap_pcp *pcp = this_cpu_ptr(vmap_pcp);
    irq_flags_t flags = spinlock_lock_irqsave(&pcp->lock);
    if (pcp->bin_count[bin] < PCP_RANGE_THRESHOLD) {
      /*
       * Only unmap the actual data, keeping the guard page unmapped.
       * Note: va_end includes the guard page.
       */
      size_t pages = (va->va_end - va->va_start - PAGE_SIZE) >> PAGE_SHIFT;
      vmm_unmap_pages(&init_mm, va->va_start, pages);
      vmm_tlb_shootdown(&init_mm, va->va_start, va->va_end);

      struct vmap_node *vn = &vmap_nodes[va->nid];
      spinlock_lock(&vn->lock);
      rb_erase_augmented(&va->rb_node, &vn->root, &vmap_area_rb_callbacks);
      list_del_rcu(&va->list);
      RB_CLEAR_NODE(&va->rb_node);
      spinlock_unlock(&vn->lock);

      list_add(&va->list, &pcp->bins[bin]);
      pcp->bin_count[bin]++;
      spinlock_unlock_irqrestore(&pcp->lock, flags);
      return;
    }
    spinlock_unlock_irqrestore(&pcp->lock, flags);
  }

  struct vmap_node *vn = &vmap_nodes[va->nid];
  irq_flags_t flags = spinlock_lock_irqsave(&vn->lock);

  /*
   * Add to purge list for lazy unmapping and TLB shootdown.
   * Do NOT remove from RB-tree yet! We must keep the VA range reserved
   * until the TLB is actually flushed to prevent Use-After-Free/Corruption.
   */
  va->flags |= VMAP_AREA_LAZY;
  list_add(&va->purge_list, &vn->purge_list);
  atomic_long_add((va->va_end - va->va_start) >> PAGE_SHIFT, &vn->nr_purged);
  spinlock_unlock_irqrestore(&vn->lock, flags);
}

EXPORT_SYMBOL(vfree);

void vfree_atomic(void *addr) { vfree(addr); }
EXPORT_SYMBOL(vfree_atomic);

/* ========================================================================
 * Standard Aliases & Init
 * ======================================================================= */

void *vmalloc_node_stack(size_t size, int nid) {
  size = PAGE_ALIGN_UP(size);
  /* Allocate extra page for the guard page (at the beginning) */
  size_t total_size = size + PAGE_SIZE;

  if (nid == NUMA_NO_NODE)
    nid = this_node();

  struct vmap_area *va = alloc_vmap_area(
      total_size, PAGE_SIZE, VMALLOC_VIRT_BASE, VMALLOC_VIRT_END, nid);
  if (!va)
    return NULL;

  /*
   * Map only the stack part, leaving the first page (guard page) unmapped.
   * This will cause a page fault (panic in kernel) on stack overflow.
   */
  uint64_t stack_start = va->va_start + PAGE_SIZE;
  uint64_t pgprot = PTE_PRESENT | PTE_RW | PTE_NX | PTE_GLOBAL;

  uint64_t cur_vaddr = stack_start;
  size_t remaining_pages = size >> PAGE_SHIFT;

  while (remaining_pages > 0) {
    struct folio *folio = alloc_pages_node(nid, GFP_KERNEL, 0);
    if (!folio) {
      vfree((void *)va->va_start);
      return NULL;
    }
    vmm_map_page_no_flush(&init_mm, cur_vaddr, folio_to_phys(folio), pgprot);
    cur_vaddr += PAGE_SIZE;
    remaining_pages--;
  }

  vmm_tlb_shootdown(&init_mm, stack_start, stack_start + size);
  return (void *)stack_start;
}

EXPORT_SYMBOL(vmalloc_node_stack);

void *vmalloc(size_t size) { return vmalloc_node(size, NUMA_NO_NODE); }
EXPORT_SYMBOL(vmalloc);

void *vzalloc(size_t size) {
  void *p = vmalloc(size);
  if (p)
    memset(p, 0, size);
  return p;
}

EXPORT_SYMBOL(vzalloc);

void *vmalloc_node(size_t size, int nid) {
  return vmalloc_node_prot(size, nid,
                           PTE_PRESENT | PTE_RW | PTE_NX | PTE_GLOBAL);
}

EXPORT_SYMBOL(vmalloc_node);

void *vmalloc_exec(size_t size) {
  return vmalloc_node_prot(size, NUMA_NO_NODE,
                           PTE_PRESENT | PTE_RW | PTE_GLOBAL);
}
EXPORT_SYMBOL(vmalloc_exec);

void vmalloc_init(void) {
  vmap_area_cachep =
      kmem_cache_create("vmap_area", sizeof(struct vmap_area), 0, 0);
  vmap_block_cachep =
      kmem_cache_create("vmap_block", sizeof(struct vmap_block), 0, 0);

  for (int i = 0; i < MAX_NUMNODES; i++) {
    spinlock_init(&vmap_nodes[i].lock);
    vmap_nodes[i].root = RB_ROOT;
    INIT_LIST_HEAD(&vmap_nodes[i].list);
    INIT_LIST_HEAD(&vmap_nodes[i].purge_list);
    atomic_long_set(&vmap_nodes[i].nr_purged, 0);
    vmap_nodes[i].nid = i;
  }

  int cpu;
  for_each_possible_cpu(cpu) {
    struct vmap_pcp *pcp = per_cpu_ptr(vmap_pcp, cpu);
    spinlock_init(&pcp->lock);
    INIT_LIST_HEAD(&pcp->free_va);
    for (int b = 0; b < PCP_BINS; b++)
      INIT_LIST_HEAD(&pcp->bins[b]);
    struct vmap_block_queue *vbq = per_cpu_ptr(vmap_block_queues, cpu);
    spinlock_init(&vbq->lock);
    INIT_LIST_HEAD(&vbq->free);
  }

  printk(KERN_INFO VMM_CLASS "vmalloc: hybrid vmalloc initialized\n");
}

void kvmap_purged_init(void) {
  struct task_struct *t =
      kthread_create(kvmap_purged_thread, NULL, "kvmap_purged");
  if (t)
    kthread_run(t);
  printk(KERN_INFO VMM_CLASS "kvmap_purged started\n");
}

void *ioremap_prot(uint64_t phys_addr, size_t size, uint64_t pgprot) {
  uint64_t offset = phys_addr & ~PAGE_MASK;
  uint64_t phys_start = phys_addr & PAGE_MASK;
  size_t page_aligned_size = PAGE_ALIGN_UP(size + offset);
  struct vmap_area *va =
      alloc_vmap_area(page_aligned_size, PAGE_SIZE, VMALLOC_VIRT_BASE,
                      VMALLOC_VIRT_END, NUMA_NO_NODE);
  if (!va)
    return NULL;
  va->flags |= VMAP_AREA_STATIC;

  /* Ensure hardware mappings are always marked global and non-executable */
  pgprot |= PTE_GLOBAL | PTE_NX;

  vmm_map_pages_no_flush(&init_mm, va->va_start, phys_start,
                         page_aligned_size / PAGE_SIZE, pgprot);
  vmm_tlb_shootdown(&init_mm, va->va_start, va->va_start + page_aligned_size);
  return (void *)(va->va_start + offset);
}

EXPORT_SYMBOL(ioremap_prot);

void *ioremap(uint64_t phys_addr, size_t size) {
  return ioremap_prot(phys_addr, size, PTE_PRESENT | PTE_RW | VMM_CACHE_UC);
}

EXPORT_SYMBOL(ioremap);

void *ioremap_wc(uint64_t phys_addr, size_t size) {
  return ioremap_prot(phys_addr, size, PTE_PRESENT | PTE_RW | VMM_CACHE_WC);
}

EXPORT_SYMBOL(ioremap_wc);

void *ioremap_wt(uint64_t phys_addr, size_t size) {
  return ioremap_prot(phys_addr, size, PTE_PRESENT | PTE_RW | VMM_CACHE_WT);
}

EXPORT_SYMBOL(ioremap_wt);

void *ioremap_wb(uint64_t phys_addr, size_t size) {
  return ioremap_prot(phys_addr, size, PTE_PRESENT | PTE_RW | VMM_CACHE_WB);
}

EXPORT_SYMBOL(ioremap_wb);

void iounmap(void *addr) { vfree(addr); }

EXPORT_SYMBOL(iounmap);

void *vmap(struct page **pages, unsigned int count, unsigned long flags,
           uint64_t pgprot) {
  (void)flags;
  size_t size = (size_t)count << PAGE_SHIFT;
  struct vmap_area *va = alloc_vmap_area(size, PAGE_SIZE, VMALLOC_VIRT_BASE,
                                         VMALLOC_VIRT_END, NUMA_NO_NODE);
  if (!va)
    return NULL;
  for (unsigned int i = 0; i < count; i++)
    vmm_map_page_no_flush(&init_mm, va->va_start + (i << PAGE_SHIFT),
                          page_to_phys(pages[i]), pgprot | PTE_GLOBAL);
  return (void *)va->va_start;
}

EXPORT_SYMBOL(vmap);

void vunmap(void *addr) { vfree(addr); }
EXPORT_SYMBOL(vunmap);

void vmalloc_dump(void) {
  printk(KERN_INFO VMM_CLASS "vmalloc state dump:\n");
  for (int i = 0; i < MAX_NUMNODES; i++) {
    struct vmap_node *vn = &vmap_nodes[i];
    irq_flags_t flags = spinlock_lock_irqsave(&vn->lock);
    int count = 0;
    unsigned long total = 0;
    struct vmap_area *va;
    list_for_each_entry(va, &vn->list, list) {
      count++;
      total += (va->va_end - va->va_start);
    }
    spinlock_unlock_irqrestore(&vn->lock, flags);
    if (count > 0) {
      printk(KERN_INFO VMM_CLASS "  Node %d: %d areas, %lu MB\n", i, count,
             total >> 20);
    }
  }
}

EXPORT_SYMBOL(vmalloc_dump);

void vmalloc_test(void) {
  printk(KERN_INFO VMM_CLASS "Starting vmalloc stress test...\n");
  void *p[100];
  for (int i = 0; i < 100; i++)
    p[i] = vmalloc(4096 * (1 + (i % 4)));
  for (int i = 0; i < 100; i++)
    vfree(p[i]);
  printk(KERN_INFO VMM_CLASS "vmalloc stress test passed\n");
}

EXPORT_SYMBOL(vmalloc_test);
