///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/slab.c
 * @brief Advanced SLUB allocator implementation
 * @copyright (C) 2025 assembler-0
*
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <mm/slab.h>
#include <mm/gfp.h>
#include <mm/page.h>
#include <mm/zone.h>
#include <mm/vmalloc.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/smp.h>
#include <linux/container_of.h>
#include <aerosync/fkx/fkx.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <aerosync/panic.h>
#include <aerosync/classes.h>
#include <aerosync/sched/sched.h>

static LIST_HEAD(slab_caches);
static spinlock_t slab_lock = 0;

static kmem_cache_t *kmalloc_caches[15];

/* Advanced SLUB/Magazine Hybrid with NUMA-awareness */

/* 
 * Slab Merging - Optimization for VFS/FD 
 * If a cache with the same size/align/flags exists, reuse it.
 */
static kmem_cache_t *find_mergeable(size_t size, size_t align, unsigned long flags) {
  kmem_cache_t *s;

  if (flags & (SLAB_POISON | SLAB_RED_ZONE)) return NULL;

  list_for_each_entry(s, &slab_caches, list) {
    if (s->object_size == (int) size && s->align == (int) align && s->flags == flags) {
      return s;
    }
  }
  return NULL;
}

/*
 * ABA Prevention TID
 * Incrementing tid on every transition.
 */
static inline unsigned long next_tid(unsigned long tid) {
  /* Prevent TID overflow by using only lower 48 bits and wrapping safely */
  tid = (tid + 1) & 0xFFFFFFFFFFFFULL;
  /* Skip 0 to avoid confusion with uninitialized state */
  return tid ? tid : 1;
}

static inline int kmalloc_index(size_t size) {
  if (!size) return 0;
  if (size <= 8) return 0;
  if (size <= 16) return 1;
  if (size <= 32) return 2;
  if (size <= 64) return 3;
  if (size <= 128) return 4;
  if (size <= 256) return 5;
  if (size <= 512) return 6;
  if (size <= 1024) return 7;
  if (size <= 2048) return 8;
  if (size <= 4096) return 9;
  if (size <= 8192) return 10;
  if (size <= 16384) return 11;
  if (size <= 32768) return 12;
  if (size <= 65536) return 13;
  if (size <= 131072) return 14;
  return -1;
}

/*
 * SLUB Hardening and Debugging
 */
#define POISON_FREE 0x6b
#define POISON_ALLOC 0xa5

static void poison_obj(kmem_cache_t *s, void *obj, uint8_t val) {
  memset(obj, val, s->object_size);
}

static void check_poison(kmem_cache_t *s, void *obj) {
  uint8_t *p = obj;
  for (int i = 0; i < s->object_size; i++) {
    /* Skip freelist pointer if it's within the object */
    if (i >= s->offset && i < s->offset + (int) sizeof(void *))
      continue;

    if (p[i] != POISON_FREE) {
      panic("SLUB: Use-after-free detected in %s at %p (offset %d, val 0x%02x)\n",
            s->name, obj, i, p[i]);
    }
  }
}

static void set_redzone(kmem_cache_t *s, void *obj) {
  if (!(s->flags & SLAB_RED_ZONE)) return;
  uint64_t *redzone = (uint64_t *) ((char *) obj + s->inuse);
  *redzone = 0xdeadbeefdeadbeef;
}

static void check_redzone(kmem_cache_t *s, void *obj) {
  if (!(s->flags & SLAB_RED_ZONE)) return;
  uint64_t *redzone = (uint64_t *) ((char *) obj + s->inuse);
  if (*redzone != 0xdeadbeefdeadbeef) {
    panic("SLUB: Redzone corruption detected in %s at %p\n", s->name, obj);
  }
}

/* Helper to get the next object in the freelist */
static inline void *get_freelist_next(void *obj, int offset) {
  return *(void **) ((char *) obj + offset);
}

static inline void set_freelist_next(void *obj, int offset, void *next) {
  *(void **) ((char *) obj + offset) = next;
}

/*
 * Double-width cmpxchg on absolute address.
 * Targets two adjacent 64-bit values (16 bytes total).
 * Must be 16-byte aligned.
 */
static inline bool cmpxchg16b_local(void *ptr, void *o1, unsigned long o2, void *n1, unsigned long n2) {
  bool ret;
  asm volatile("lock; cmpxchg16b %1; setz %0"
    : "=a"(ret), "+m"(*(char *) ptr), "+d"(o2), "+a"(o1)
    : "b"(n1), "c"(n2)
    : "memory");
  return ret;
}

static struct page *allocate_slab(kmem_cache_t *s, gfp_t flags, int node) {
  struct folio *folio;
  struct page *page;
  void *start;
  void *p;

  if (node == -1) node = this_node();

  folio = alloc_pages_node(node, flags, s->order);
  if (!folio)
    return NULL;

  page = &folio->page;
  start = page_address(page);
  page->slab_cache = s;
  page->objects = (unsigned short) ((PAGE_SIZE << s->order) / s->size);
  page->inuse = 0;
  page->frozen = 0;
  page->node = node;
  SetPageSlab(page);
  INIT_LIST_HEAD(&page->list);

  /* Build freelist */
  page->freelist = start;
  for (int i = 0; i < (int) page->objects - 1; i++) {
    p = (char *) start + i * s->size;
    if (s->flags & SLAB_POISON) poison_obj(s, p, POISON_FREE);
    set_redzone(s, p);
    set_freelist_next(p, s->offset, (char *) p + s->size);
  }

  /* Last object */
  p = (char *) start + (page->objects - 1) * s->size;
  if (s->flags & SLAB_POISON) poison_obj(s, p, POISON_FREE);
  set_redzone(s, p);
  set_freelist_next(p, s->offset, NULL);

  return page;
}

/* Slowpath allocation */
static void *__slab_alloc(kmem_cache_t *s, gfp_t gfpflags, int node, struct kmem_cache_cpu *c) {
  void *freelist;
  struct page *page;
  irq_flags_t flags;

  if (node == -1) node = this_node();

  /* 
   * We need to protect the CPU structure during the slowpath.
   * Note: c is a pointer to the per-cpu structure for the CURRENT cpu.
   */
  flags = save_irq_flags();

  /* Check if we have a page but no freelist (slab frozen) */
  page = c->page;
  if (!page)
    goto find_slab;

  /* If page is from wrong node, unfreeze it and get a new one */
  if (node != -1 && page->node != node) {
    page->frozen = 0;
    c->page = NULL;
    c->freelist = NULL;
    goto find_slab;
  }

  /* Try to get objects from the frozen page's freelist */
  struct kmem_cache_node *n = s->node[page->node];
  spinlock_lock(&n->list_lock);
  freelist = page->freelist;
  if (freelist) {
    page->freelist = NULL; /* Take the whole freelist */
    page->inuse = (unsigned short) page->objects;
    spinlock_unlock(&n->list_lock);
    c->freelist = get_freelist_next(freelist, s->offset);
    c->tid = next_tid(c->tid);
    restore_irq_flags(flags);
    return freelist;
  }
  spinlock_unlock(&n->list_lock);
  /* Page is full, unfreeze it */
  page->frozen = 0;
  c->page = NULL;

find_slab:;
  struct kmem_cache_node *target_node = s->node[node];
  spinlock_lock(&target_node->list_lock);
  if (!list_empty(&target_node->partial)) {
    page = list_first_entry(&target_node->partial, struct page, list);
    list_del(&page->list);
    target_node->nr_partial--;
    spinlock_unlock(&target_node->list_lock);
    goto freeze;
  }
  spinlock_unlock(&target_node->list_lock);

  /* NUMA Fallback: Check other nodes */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (i == node) continue;
    target_node = s->node[i];
    if (!target_node) continue;

    spinlock_lock(&target_node->list_lock);
    if (!list_empty(&target_node->partial)) {
      page = list_first_entry(&target_node->partial, struct page, list);
      list_del(&page->list);
      target_node->nr_partial--;
      spinlock_unlock(&target_node->list_lock);
      goto freeze;
    }
    spinlock_unlock(&target_node->list_lock);
  }

  /* Allocate new slab */
  page = allocate_slab(s, gfpflags, node);
  if (!page) {
    restore_irq_flags(flags);
    return NULL;
  }
  atomic_long_inc(&s->active_slabs);

freeze:
  page->frozen = 1;
  c->page = page;
  freelist = page->freelist;
  page->freelist = NULL;
  page->inuse = (unsigned short) page->objects;
  c->freelist = get_freelist_next(freelist, s->offset);
  c->tid = next_tid(c->tid);

  restore_irq_flags(flags);
  return freelist;
}

static void rcu_free_slab_callback(struct rcu_head *head);

static void __free_slab(kmem_cache_t *s, struct page *page) {
  ClearPageSlab(page);

  if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU)) {
    struct rcu_head *head = (struct rcu_head *) &page->list;
    call_rcu(head, rcu_free_slab_callback);
  } else {
    __free_pages(page, s->order);
  }
}

static void rcu_free_slab_callback(struct rcu_head *head) {
  struct page *page = (struct page *) ((char *) head - offsetof(struct page, list));
  kmem_cache_t *s = page->slab_cache;
  __free_slab(s, page);
}

/* 
 * Refill the CPU magazine from the partial list or a new slab.
 * Must be called with IRQs disabled and returns one object.
 */
static void *refill_magazine(kmem_cache_t *s, struct kmem_cache_cpu *c, int node) {
  void *object = NULL;
  struct kmem_cache_node *n;
  struct page *page;

  if (node == -1) node = this_node();
  n = s->node[node];

  spinlock_lock(&n->list_lock);
  if (!list_empty(&n->partial)) {
    page = list_first_entry(&n->partial, struct page, list);

    /* Take as many objects as we can for the magazine */
    while (c->mag_count < SLAB_MAG_SIZE && page->freelist) {
      void *obj = page->freelist;
      page->freelist = get_freelist_next(obj, s->offset);
      page->inuse++;
      c->mag[c->mag_count++] = obj;
    }

    if (!page->freelist) {
      list_del(&page->list);
      n->nr_partial--;
    }
    spinlock_unlock(&n->list_lock);

    if (c->mag_count > 0) {
      return c->mag[--c->mag_count];
    }
  } else {
    spinlock_unlock(&n->list_lock);
  }

  /* Fallback to normal slab allocation if magazine couldn't be refilled from partials */
  return __slab_alloc(s, GFP_KERNEL, node, c);
}

void *kmem_cache_alloc_node(kmem_cache_t *s, int node) {
  void *object;
  struct kmem_cache_cpu *c;
  unsigned long tid;
  int cpu;

redo:
  /* Fastpath: Lockless using cmpxchg16b and TIDs */
  preempt_disable();
  cpu = smp_get_id();
  c = &s->cpu_slab[cpu];
  tid = c->tid;
  object = c->freelist;

  /* 
   * If we have an object, it's from the local node (or we don't care about the node),
   * try to take it atomically.
   */
  if (likely(object && (node == -1 || (c->page && c->page->node == node)))) {
    void *next = get_freelist_next(object, s->offset);
    if (unlikely(!cmpxchg16b_local(c, object, tid, next, next_tid(tid)))) {
      preempt_enable();
      goto redo;
    }
    preempt_enable();
  } else {
    preempt_enable();
    /* Magazine Layer (Hot Path) - Only for local node allocations */
    if (node == -1 || node == this_node()) {
      irq_flags_t flags = save_irq_flags();
      c = &s->cpu_slab[smp_get_id()];
      if (c->mag_count > 0) {
        object = c->mag[--c->mag_count];
        restore_irq_flags(flags);
        goto found;
      }

      /* Refill Magazine from partial lists */
      object = refill_magazine(s, c, node);
      restore_irq_flags(flags);
      if (object) goto found;
    }

    /* Slowpath: Either no objects or wrong node */
    object = __slab_alloc(s, GFP_KERNEL, node, c);
  }

found:
  if (object) {
    if (s->flags & SLAB_POISON) check_poison(s, object);
    check_redzone(s, object);
    atomic_long_inc(&s->total_objects);
  }

  return object;
}

void *kmem_cache_alloc(kmem_cache_t *s) {
  return kmem_cache_alloc_node(s, -1);
}

static void __slab_free(kmem_cache_t *s, struct page *page, void *x) {
  irq_flags_t flags;
  void *prior;
  int was_frozen;
  struct kmem_cache_node *n = s->node[page->node];

  flags = spinlock_lock_irqsave(&n->list_lock);

  prior = page->freelist;
  set_freelist_next(x, s->offset, prior);
  page->freelist = x;
  page->inuse--;

  was_frozen = (int) page->frozen;

  if (unlikely(!page->inuse)) {
    /* Optimization: don't free if we are below min_partial or it's the last slab */
    if (!was_frozen && n->nr_partial > s->min_partial) {
      if (prior) {
        list_del(&page->list);
        n->nr_partial--;
      }
      spinlock_unlock_irqrestore(&n->list_lock, flags);
      __free_slab(s, page);
      atomic_long_dec(&s->active_slabs);
      return;
    }
  }

  /* Slab was full, now it has one free object, add to partial list */
  if (!was_frozen && !prior) {
    list_add(&page->list, &n->partial);
    n->nr_partial++;
  }

  spinlock_unlock_irqrestore(&n->list_lock, flags);
}

void kmem_cache_free(kmem_cache_t *s, void *x) {
  struct page *page;
  struct kmem_cache_cpu *c;
  unsigned long tid;
  int cpu;

  if (unlikely(!x)) return;

  check_redzone(s, x);
  if (s->flags & SLAB_POISON) poison_obj(s, x, POISON_FREE);

  page = virt_to_head_page(x);
  atomic_long_dec(&s->total_objects);

redo:
  preempt_disable();
  cpu = smp_get_id();
  c = &s->cpu_slab[cpu];
  tid = c->tid;

  /* Fastpath: Return to current CPU's frozen slab */
  if (likely(page == c->page)) {
    void *prior = c->freelist;
    set_freelist_next(x, s->offset, prior);
    if (unlikely(!cmpxchg16b_local(c, prior, tid, x, next_tid(tid)))) {
      preempt_enable();
      goto redo;
    }
    preempt_enable();
  } else {
    preempt_enable();
    /* Magazine Layer (Hot Path) - Only for local node objects */
    if (page->node == this_node()) {
      irq_flags_t flags = save_irq_flags();
      c = &s->cpu_slab[smp_get_id()];
      if (c->mag_count < SLAB_MAG_SIZE) {
        c->mag[c->mag_count++] = x;
        restore_irq_flags(flags);
        return;
      }
      restore_irq_flags(flags);
    }

    /* Slowpath: Different slab or magazine full */
    __slab_free(s, page, x);
  }
}

static void init_kmem_cache_node(struct kmem_cache_node *n) {
  spinlock_init(&n->list_lock);
  n->nr_partial = 0;
  INIT_LIST_HEAD(&n->partial);
}

static void init_kmem_cache_cpu(struct kmem_cache_cpu *c) {
  c->freelist = NULL;
  c->page = NULL;
  c->tid = 0;
}

#define ALIGNED_MAGIC 0xDEADBEEFCAFEBABE

void *kmalloc_aligned(size_t size, size_t align) {
  if (align <= 8) return kmalloc(size);

  // For larger alignments, allocate extra space for alignment, magic and original pointer
  size_t total_size = size + align - 1 + 2 * sizeof(void *);
  void *raw = kmalloc(total_size);
  if (!raw) return NULL;

  // Calculate aligned address, ensuring at least 16 bytes for metadata
  uintptr_t aligned = (uintptr_t) raw + 2 * sizeof(void *);
  aligned = (aligned + align - 1) & ~(align - 1);

  // Store magic and original pointer before aligned address
  void **magic_ptr = (void **) (aligned - 2 * sizeof(void *));
  *magic_ptr = (void *) ALIGNED_MAGIC;
  void **orig_ptr = (void **) (aligned - sizeof(void *));
  *orig_ptr = raw;

  return (void *) aligned;
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags) {
  kmem_cache_t *s;

  spinlock_lock(&slab_lock);
  s = find_mergeable(size, align, flags);
  if (s) {
    spinlock_unlock(&slab_lock);
    return s;
  }
  spinlock_unlock(&slab_lock);

  s = kmalloc(sizeof(kmem_cache_t));
  if (!s) return NULL;

  memset(s, 0, sizeof(kmem_cache_t));
  s->name = name;
  s->object_size = (int) size;
  s->align = (int) align;
  s->flags = flags;

  /* Calculate size with alignment and meta-data */
  if (align < 8) align = 8;
  size = (size + align - 1) & ~(align - 1);
  s->inuse = (int) size;

  if (flags & SLAB_RED_ZONE) {
    size += sizeof(uint64_t); /* Redzone */
  }

  if (size < sizeof(void *)) size = sizeof(void *);
  s->size = (int) size;
  s->offset = 0;

  /* Determine order - aim for at least 8 objects per slab */
  s->order = 0;
  while ((PAGE_SIZE << s->order) < (size_t) size * 8 && s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = 5;

  /* 
   * Allocate per-CPU slabs - ensure 16-byte alignment for cmpxchg16b.
   * Note: We use a manual array for now as we lack alloc_percpu().
   */
  s->cpu_slab = kmalloc_aligned(sizeof(struct kmem_cache_cpu) * MAX_CPUS, 16);
  if (!s->cpu_slab) {
    kfree(s);
    return NULL;
  }

  memset(s->cpu_slab, 0, sizeof(struct kmem_cache_cpu) * MAX_CPUS);

  for (int i = 0; i < MAX_CPUS; i++) {
    init_kmem_cache_cpu(&s->cpu_slab[i]);
  }

  /* Allocate nodes */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (node_data[i]) {
      s->node[i] = kmalloc_node(sizeof(struct kmem_cache_node), i);
    } else {
      s->node[i] = kmalloc(sizeof(struct kmem_cache_node));
    }

    if (!s->node[i]) continue;
    init_kmem_cache_node(s->node[i]);
  }

  spinlock_lock(&slab_lock);
  list_add(&s->list, &slab_caches);
  spinlock_unlock(&slab_lock);

  return s;
}

/*
 * The Boot Cache problem:
 * If VFS starts before kmalloc is fully dynamic, we need more capacity.
 */
#define BOOT_CACHES_MAX 64
static kmem_cache_t static_caches[BOOT_CACHES_MAX];
static struct kmem_cache_cpu static_cpu_slabs[BOOT_CACHES_MAX][MAX_CPUS];
static struct kmem_cache_node static_nodes[BOOT_CACHES_MAX][MAX_NUMNODES];
static int static_idx = 0;

static struct kmem_cache *create_boot_cache(const char *name, size_t size, unsigned long flags) {
  if (static_idx >= BOOT_CACHES_MAX) {
    panic("SLUB: Out of boot caches (%d used). Increase BOOT_CACHES_MAX!", static_idx);
  }

  kmem_cache_t *s = &static_caches[static_idx];
  memset(s, 0, sizeof(kmem_cache_t));

  s->name = name;
  s->object_size = (int) size;
  s->size = (int) size;

  /* Enforce alignment: 8 bytes for small, 16 bytes for >= 16 to support cmpxchg16b */
  if (size < 8) s->align = 8;
  else if (size < 16) s->align = 8;
  else s->align = 16;

  s->flags = flags;
  s->inuse = (int) size;

  s->order = 0;
  while ((PAGE_SIZE << s->order) < (size_t) size * 4 && s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = 2;
  s->cpu_slab = static_cpu_slabs[static_idx];
  for (int i = 0; i < MAX_CPUS; i++) {
    init_kmem_cache_cpu(&s->cpu_slab[i]);
  }

  for (int i = 0; i < MAX_NUMNODES; i++) {
    s->node[i] = &static_nodes[static_idx][i];
    init_kmem_cache_node(s->node[i]);
  }

  static_idx++;
  list_add(&s->list, &slab_caches);
  return s;
}

void slab_init(void) {
  char *names[] = {
    "kmalloc-8", "kmalloc-16", "kmalloc-32", "kmalloc-64",
    "kmalloc-128", "kmalloc-256", "kmalloc-512", "kmalloc-1k",
    "kmalloc-2k", "kmalloc-4k", "kmalloc-8k", "kmalloc-16k",
    "kmalloc-32k", "kmalloc-64k", "kmalloc-128k"
  };

  INIT_LIST_HEAD(&slab_caches);

  for (int i = 0; i < 15; i++) {
    size_t size = 8 << i;
    unsigned long flags = SLAB_HWCACHE_ALIGN;
    kmalloc_caches[i] = create_boot_cache(names[i], size, flags);
  }

  printk(SLAB_CLASS "SLUB Hybrid initialized (%d caches, Magazine size %d)\n", 15, SLAB_MAG_SIZE);
}

void *kmalloc_node(size_t size, int node) {
  if (unlikely(size > SLAB_MAX_SIZE))
    return vmalloc(size);

  int idx = kmalloc_index(size);
  if (unlikely(idx < 0)) return NULL;

  return kmem_cache_alloc_node(kmalloc_caches[idx], node);
}

void *kmalloc(size_t size) {
  return kmalloc_node(size, -1);
}

void *kzalloc_node(size_t size, int node) {
  void *ptr = kmalloc_node(size, node);
  if (ptr && size <= SLAB_MAX_SIZE) {
    memset(ptr, 0, size);
  }
  return ptr;
}

void *kzalloc(size_t size) {
  return kzalloc_node(size, -1);
}

void kfree(void *ptr) {
  struct page *page;
  kmem_cache_t *s;

  if (unlikely(!ptr)) return;

  if ((uintptr_t) ptr >= VMALLOC_VIRT_BASE && (uintptr_t) ptr < VMALLOC_VIRT_END) {
    vfree(ptr);
    return;
  }

  page = virt_to_head_page(ptr);
  if (unlikely(!PageSlab(page))) {
    return;
  }

  s = page->slab_cache;

  /*
   * Check if this is an aligned allocation.
   * Aligned allocations (from kmalloc_aligned) have a magic value and original pointer.
   *
   * FIX: Check if ptr is at the start of the slab allocation.
   * Aligned pointers always have a header before them, so they cannot be
   * at the very beginning of the slab. This check prevents accessing invalid
   * memory at (ptr - 16) when ptr is at a page boundary.
   */
  if (ptr != page_address(page)) {
    void *magic = *((void **) ((uintptr_t) ptr - 2 * sizeof(void *)));
    if (unlikely(magic == (void *) ALIGNED_MAGIC)) {
      void *raw = *((void **) ((uintptr_t) ptr - sizeof(void *)));
      kfree(raw);
      return;
    }
  }

  kmem_cache_free(s, ptr);
}

void slab_test(void) {
  printk(KERN_DEBUG SLAB_CLASS "Starting SLUB Stress Test...\n");

  /* Test 1: Basic kmalloc/kfree */
  void *p = kmalloc(32);
  if (!p) panic(SLAB_CLASS "kmalloc(32) failed");
  memset(p, 0xAA, 32);
  kfree(p);
  printk(KERN_DEBUG SLAB_CLASS "  - Basic Alloc/Free: OK\n");

  /* Test 2: Array Allocation (Stress) */
#define TEST_COUNT 1024
  void **ptrs = kmalloc(TEST_COUNT * sizeof(void *));
  if (!ptrs) panic(SLAB_CLASS "failed to allocate pointer array");

  for (int i = 0; i < TEST_COUNT; i++) {
    ptrs[i] = kmalloc(64);
    if (!ptrs[i]) panic(SLAB_CLASS "stress alloc failed");
    memset(ptrs[i], (i & 0xFF), 64);
  }

  /* Verify patterns */
  for (int i = 0; i < TEST_COUNT; i++) {
    uint8_t val = (i & 0xFF);
    uint8_t *byte_ptr = (uint8_t *) ptrs[i];
    for (int j = 0; j < 64; j++) {
      if (byte_ptr[j] != val) {
        panic(SLAB_CLASS "data corruption detected");
      }
    }
  }

  /* Free in reverse order */
  for (int i = TEST_COUNT - 1; i >= 0; i--) {
    kfree(ptrs[i]);
  }
  kfree(ptrs);
  printk(KERN_DEBUG SLAB_CLASS "  - Stress Alloc (1024 objects): OK\n");

  /* Test 3: Large Allocations */
  void *large = kmalloc(128 * 1024); /* 128KB (Max Slab) */
  if (!large) panic(SLAB_CLASS "large alloc failed");
  memset(large, 0xBB, 128 * 1024);
  kfree(large);
  printk(KERN_DEBUG SLAB_CLASS "  - Large Alloc (128KB): OK\n");

  /* Test 4: Custom Cache with Hardening */
  kmem_cache_t *custom = kmem_cache_create("test-cache", 150, 8, SLAB_POISON | SLAB_RED_ZONE);
  if (!custom) panic(SLAB_CLASS "cache create failed");

  void *obj1 = kmem_cache_alloc(custom);
  void *obj2 = kmem_cache_alloc(custom);
  if (!obj1 || !obj2) panic(SLAB_CLASS "cache alloc failed");

  if (obj1 == obj2) panic(SLAB_CLASS "duplicate object returned!");

  kmem_cache_free(custom, obj1);
  kmem_cache_free(custom, obj2);
  printk(KERN_DEBUG SLAB_CLASS "  - Custom Cache with Hardening: OK\n");

  /* Test 5: Aligned Allocations */
  void *a1 = kmalloc_aligned(16, 64);
  if (!a1) panic(SLAB_CLASS "aligned alloc 1 failed");
  if ((uintptr_t) a1 & 63) panic(SLAB_CLASS "aligned alloc not 64-byte aligned!");
  kfree(a1);

  void *a2 = kmalloc_aligned(1024, 4096);
  if (!a2) panic(SLAB_CLASS "aligned alloc 2 failed");
  if ((uintptr_t) a2 & 4095) panic(SLAB_CLASS "aligned alloc not 4096-byte aligned!");
  kfree(a2);
  printk(KERN_DEBUG SLAB_CLASS "  - Aligned Allocations: OK\n");

  /* Test 6: NUMA-aware Allocations */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (!node_data[i]) continue;
    void *n_ptr = kmalloc_node(128, i);
    if (!n_ptr) panic(SLAB_CLASS "kmalloc_node failed");
    struct page *p = virt_to_head_page(n_ptr);
    if (p->node != (uint32_t) i) {
      printk(KERN_WARNING SLAB_CLASS "Object not on requested node %d (on %d)\n", i, p->node);
    }
    kfree(n_ptr);
  }
  printk(KERN_DEBUG SLAB_CLASS "  - NUMA-aware Allocations: OK\n");

  printk(KERN_DEBUG SLAB_CLASS "SLUB Stress Test Passed.\n");
}

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(kmem_cache_alloc);
EXPORT_SYMBOL(kmem_cache_free);
EXPORT_SYMBOL(kmem_cache_create);
EXPORT_SYMBOL(slab_test);
