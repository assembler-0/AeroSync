/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/slub.c
 * @brief Advanced SLUB allocator implementation
 * @copyright (C) 2025-2026 assembler-0
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

#include <aerosync/classes.h>
#include <aerosync/fkx/fkx.h>
#include <aerosync/panic.h>
#include <aerosync/timer.h>
#include <aerosync/sched/sched.h>
#include <arch/x86_64/mm/layout.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/smp.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/container_of.h>
#include <mm/gfp.h>
#include <mm/page.h>
#include <mm/slub.h>
#include <mm/vmalloc.h>
#include <mm/zone.h>
#include <aerosync/crypto.h>
#include <mm/ssp.h>

static LIST_HEAD(slab_caches);
static DEFINE_SPINLOCK(slab_lock);
static uint64_t slab_secret = 0;

static kmem_cache_t *kmalloc_caches[15];

/* Advanced SLUB/Magazine Hybrid with NUMA-awareness */

static kmem_cache_t *find_mergeable(size_t size, size_t align,
                                    unsigned long flags) {
  kmem_cache_t *s;

  if (flags & (SLAB_POISON | SLAB_RED_ZONE))
    return nullptr;

  list_for_each_entry(s, &slab_caches, list) {
    if (s->object_size == (int)size && s->align == (int)align &&
        s->flags == flags) {
      return s;
    }
  }
  return nullptr;
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
  if (unlikely(!size))
    return 0;
  if (size <= 8)
    return 0;
  if (unlikely(size > 131072))
    return -EINVAL;
  return 64 - __builtin_clzll(size - 1) - 3;
}

/*
 * SLUB Hardening and Debugging
 */
#define POISON_FREE 0x6b
#define POISON_ALLOC 0xa5

static void poison_obj(kmem_cache_t *s, void *obj, uint8_t val) {
#ifdef MM_HARDENING
  memset(obj, val, s->object_size);
#else
  (void)s; (void)obj; (void)val;
#endif
}

static void check_poison(kmem_cache_t *s, void *obj) {
#ifdef MM_HARDENING
  uint8_t *p = obj;
  int i = 0;

  /* Skip freelist pointer if it's within the object */
  /* Most common case: offset 0, size 8 */
  if (s->offset == 0 && s->object_size >= (int)sizeof(void *)) {
    i = sizeof(void *);
  }

  /* Optimized word-at-a-time check for the rest of the object */
  uint64_t expected = 0x6b6b6b6b6b6b6b6bULL;
  while (i <= s->object_size - 8) {
    /* Skip if this word contains the freelist pointer */
    if (unlikely(i >= s->offset && i < s->offset + (int)sizeof(void *))) {
      i += sizeof(void *);
      continue;
    }

    if (unlikely(*(uint64_t *)(p + i) != expected)) {
      /* Fallback to byte-by-byte for exact panic */
      goto slow_path;
    }
    i += 8;
  }

slow_path:
  for (; i < s->object_size; i++) {
    /* Skip freelist pointer if it's within the object */
    if (i >= s->offset && i < s->offset + (int)sizeof(void *))
      continue;

    if (p[i] != POISON_FREE) {
      panic(
          "SLUB: Use-after-free detected in %s at %p (offset %d, val 0x%02x)\n",
          s->name, obj, i, p[i]
      );
    }
  }
#else
  (void)s; (void)obj;
#endif
}

static void set_redzone(kmem_cache_t *s, void *obj) {
#ifdef MM_HARDENING
  if (!(s->flags & SLAB_RED_ZONE))
    return;
  uint64_t *redzone = (uint64_t *)((char *)obj + s->inuse);
  *redzone = STACK_CANARY_VALUE;
#else
  (void)s; (void)obj;
#endif
}

static void check_redzone(kmem_cache_t *s, void *obj) {
#ifdef MM_HARDENING
  if (!(s->flags & SLAB_RED_ZONE))
    return;
  uint64_t *redzone = (uint64_t *)((char *)obj + s->inuse);
  if (unlikely(*redzone != STACK_CANARY_VALUE)) {
    panic("SLUB: Redzone corruption detected in %s at %p\n", s->name, obj);
  }
#else
  (void)s; (void)obj;
#endif
}

/* Helper to get the next object in the freelist with XOR obfuscation */
static inline void *get_freelist_next(void *obj, int offset) {
  uint64_t val = *(uint64_t *)((char *)obj + offset);
  if (!val) return nullptr;
  return (void *)(val ^ slab_secret ^ (uint64_t)obj);
}

static inline void set_freelist_next(void *obj, int offset, void *next) {
  uint64_t val = next ? ((uint64_t)next ^ slab_secret ^ (uint64_t)obj) : 0;
  *(uint64_t *)((char *)obj + offset) = val;
}

static struct page *allocate_slab(kmem_cache_t *s, gfp_t flags, int node) {
  struct folio *folio;
  struct page *page;
  void *start;
  void *p;

  if (node == -1)
    node = this_node();

  folio = alloc_pages_node(node, flags, s->order);
  if (!folio)
    return nullptr;

  page = &folio->page;
  start = page_address(page);
  page->slab_cache = s;
  page->objects = (unsigned short)((PAGE_SIZE << s->order) / s->size);
  page->inuse = 0;
  page->frozen = 0;
  page->node = node;
  SetPageSlab(page);
  INIT_LIST_HEAD(&page->list);

  /* Build freelist */
  page->freelist = start;
  for (int i = 0; i < (int)page->objects - 1; i++) {
    p = (char *)start + i * s->size;
    if (s->flags & SLAB_POISON)
      poison_obj(s, p, POISON_FREE);
    set_redzone(s, p);
    set_freelist_next(p, s->offset, (char *)p + s->size);
  }

  /* Last object */
  p = (char *)start + (page->objects - 1) * s->size;
  if (s->flags & SLAB_POISON)
    poison_obj(s, p, POISON_FREE);
  set_redzone(s, p);
  set_freelist_next(p, s->offset, nullptr);

  return page;
}

/* Slowpath allocation */
static void *__slab_alloc(kmem_cache_t *s, gfp_t gfpflags, int node,
                          struct kmem_cache_cpu *c) {
  void *freelist;
  struct page *page;
  irq_flags_t flags;

  if (node == -1)
    node = this_node();

  /*
   * We need to protect the CPU structure during the slowpath.
   * Note: c is a pointer to the per-cpu structure for the CURRENT cpu.
   */
  flags = local_irq_save();

  /* Check if we have a page but no freelist (slab frozen) */
  page = c->page;
  if (!page)
    goto find_slab;

  /* If page is from wrong node, unfreeze it and get a new one */
  if (node != -1 && page->node != node) {
    page->frozen = 0;
    c->page = nullptr;
    c->freelist = nullptr;
    goto find_slab;
  }

  /* Try to get objects from the frozen page's freelist */
  struct kmem_cache_node *n = s->node[page->node];
  spinlock_lock(&n->list_lock);
  freelist = page->freelist;
  if (freelist) {
    page->freelist = nullptr; /* Take the whole freelist */
    page->inuse = (unsigned short)page->objects;
    spinlock_unlock(&n->list_lock);
    c->freelist = get_freelist_next(freelist, s->offset);
    c->tid = next_tid(c->tid);
    local_irq_restore(flags);
    return freelist;
  }
  spinlock_unlock(&n->list_lock);
  /* Page is full, unfreeze it */
  page->frozen = 0;
  c->page = nullptr;

find_slab:;
  /*
   * NUMA-Aware Tiered Fallback Strategy:
   * 1. Try local node (distance 10)
   * 2. Try same-socket nodes (distance 20)
   * 3. Try cross-socket nodes (distance 30+)
   * 4. Allocate new slab on memory-local node
   */
  extern int numa_distance_get(int from, int to);
  extern int numa_mem_id(void); /* Get memory-local node (not CPU node) */

  int cpu_node = (node == -1) ? this_node() : node;
  int alloc_node = cpu_node;

  /* Tier 1: Local node (distance 10) */
  struct kmem_cache_node *target_node = s->node[cpu_node];
  spinlock_lock(&target_node->list_lock);
  if (!list_empty(&target_node->partial)) {
    page = list_first_entry(&target_node->partial, struct page, list);
    list_del(&page->list);
    target_node->nr_partial--;
    atomic_long_inc(&target_node->alloc_hits);
    atomic_long_inc(&target_node->alloc_from_partial);
    spinlock_unlock(&target_node->list_lock);
    alloc_node = cpu_node;
    goto freeze;
  }
  spinlock_unlock(&target_node->list_lock);

  /* Tier 2 & 3: Distance-based fallback */
  int *fallback = s->node_fallback[cpu_node];
  if (fallback) {
    for (int i = 0; fallback[i] != -1; i++) {
      int nid = fallback[i];
      target_node = s->node[nid];

      spinlock_lock(&target_node->list_lock);
      if (!list_empty(&target_node->partial)) {
        page = list_first_entry(&target_node->partial, struct page, list);
        list_del(&page->list);
        target_node->nr_partial--;
        atomic_long_inc(&target_node->alloc_from_partial);
        spinlock_unlock(&target_node->list_lock);

        /* Track cross-node allocation */
        atomic_long_inc(&s->node[cpu_node]->alloc_misses);
        atomic_long_inc(&target_node->alloc_hits);

        alloc_node = nid;
        goto freeze;
      }
      spinlock_unlock(&target_node->list_lock);
    }
  }

  /* Allocate new slab on memory-local node for best bandwidth */
  page = allocate_slab(s, gfpflags, alloc_node);
  if (!page) {
    restore_irq_flags(flags);
    return nullptr;
  }
  atomic_long_inc(&s->active_slabs);
  atomic_long_inc(&s->node[alloc_node]->alloc_refills);

freeze:
  page->frozen = 1;
  c->page = page;
  freelist = page->freelist;
  page->freelist = nullptr;
  page->inuse = (unsigned short)page->objects;
  c->freelist = get_freelist_next(freelist, s->offset);
  c->tid = next_tid(c->tid);

  local_irq_restore(flags);
  return freelist;
}

static void rcu_free_slab_callback(struct rcu_head *head);

static void __free_slab(kmem_cache_t *s, struct page *page) {
  ClearPageSlab(page);

  if (unlikely(s->flags & SLAB_TYPESAFE_BY_RCU)) {
    struct rcu_head *head = (struct rcu_head *)&page->list;
    call_rcu(head, rcu_free_slab_callback);
  } else {
    __free_pages(page, s->order);
  }
}

static void rcu_free_slab_callback(struct rcu_head *head) {
  struct page *page =
      (struct page *)((char *)head - offsetof(struct page, list));
  kmem_cache_t *s = page->slab_cache;
  __free_slab(s, page);
}

/*
 * Refill the CPU magazine from the partial list or a new slab.
 * Must be called with IRQs disabled and returns one object.
 */
static void *refill_magazine(kmem_cache_t *s, struct kmem_cache_cpu *c,
                             int node) {
  void *object = nullptr;
  struct kmem_cache_node *n;
  struct page *page;

  if (node == -1)
    node = this_node();
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

  /* Fallback to normal slab allocation if magazine couldn't be refilled from
   * partials */
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
   * If we have an object, it's from the local node (or we don't care about the
   * node), try to take it atomically.
   */
  if (likely(object && (node == -1 || (c->page && c->page->node == node)))) {
    void *next = get_freelist_next(object, s->offset);
    if (unlikely(!cmpxchg16b_local(c, object, tid, next, next_tid(tid)))) {
      atomic_long_inc(&s->alloc_fastpath);
      preempt_enable();
      goto redo;
    }
    preempt_enable();
  } else {
    preempt_enable();
    /* Magazine Layer (Hot Path) - Only for local node allocations */
    if (node == -1 || node == this_node()) {
      irq_flags_t flags = local_irq_save();
      c = &s->cpu_slab[smp_get_id()];
      if (c->mag_count > 0) {
        object = c->mag[--c->mag_count];
        local_irq_restore(flags);
        goto found;
      }

      /* Refill Magazine from partial lists */
      object = refill_magazine(s, c, node);
      local_irq_restore(flags);
      if (object)
        goto found;
    }

    /* Slowpath: Either no objects or wrong node */
    atomic_long_inc(&s->alloc_slowpath);
    object = __slab_alloc(s, GFP_KERNEL, node, c);
  }

found:
  if (object) {
    if (s->flags & SLAB_POISON)
      check_poison(s, object);
    check_redzone(s, object);
    atomic_long_inc(&s->total_objects);
  }

  return object;
}

void *kmem_cache_alloc(kmem_cache_t *s) { return kmem_cache_alloc_node(s, -1); }

static void __slab_free(kmem_cache_t *s, struct page *page, void *x) {
  void *prior;
  int was_frozen;
  struct kmem_cache_node *n = s->node[page->node];

  /*
   * Lockless Fastpath: Try to free to frozen slab without locks
   * Frozen slabs are exclusively owned by a CPU, so we can modify locklessly
   */
  if (likely(page->frozen)) {
    /* Frozen slab - lockless update */
    prior = page->freelist;
    set_freelist_next(x, s->offset, prior);

    /* Atomic update of freelist */
    if (__atomic_compare_exchange_n(&page->freelist, &prior, x, false,
                                    __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
      page->inuse--;
      atomic_long_inc(&s->free_fastpath);
      return; /* Fast path success */
    }
    /* CAS failed, fall through to slowpath */
  }

  /*
   * Slowpath: Use per-page bit-spinlock for partial/full slabs
   * This is much more fine-grained than the old node-level lock
   */
  atomic_long_inc(&s->free_slowpath);
  lock_page_slab(page);

  prior = page->freelist;
  set_freelist_next(x, s->offset, prior);
  page->freelist = x;
  page->inuse--;

  was_frozen = (int)page->frozen;

  if (unlikely(!page->inuse)) {
    /* Slab is now empty */
    if (!was_frozen && n->nr_partial > s->min_partial) {
      /* Remove from partial list and free the slab */
      unlock_page_slab(page);

      /* Need node lock to modify partial list */
      irq_flags_t flags = spinlock_lock_irqsave(&n->list_lock);
      if (prior && !page->frozen) {
        /* Re-check after acquiring lock */
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
    unlock_page_slab(page);

    /* Need node lock to add to partial list */
    irq_flags_t flags = spinlock_lock_irqsave(&n->list_lock);
    if (!page->frozen && !page->freelist) {
      /* Re-check */
      list_add(&page->list, &n->partial);
      n->nr_partial++;
    }
    spinlock_unlock_irqrestore(&n->list_lock, flags);
    return;
  }

  unlock_page_slab(page);
}

void kmem_cache_free(kmem_cache_t *s, void *x) {
  struct page *page;
  struct kmem_cache_cpu *c;
  unsigned long tid;
  int cpu;

  if (unlikely(!x))
    return;

  check_redzone(s, x);
  if (s->flags & SLAB_POISON)
    poison_obj(s, x, POISON_FREE);

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
      irq_flags_t flags = local_irq_save();
      c = &s->cpu_slab[smp_get_id()];
      if (c->mag_count < SLAB_MAG_SIZE) {
        c->mag[c->mag_count++] = x;
        local_irq_restore(flags);
        return;
      }
      local_irq_restore(flags);
    }

    /* Slowpath: Different slab or magazine full */
    __slab_free(s, page, x);
  }
}

int kmem_cache_alloc_bulk(kmem_cache_t *s, gfp_t flags, size_t size, void **p) {
  if (!size)
    return 0;

  /* Attempt fastpath bulk allocation */
  struct kmem_cache_cpu *c;
  unsigned long tid;
  int cpu;

  preempt_disable();
  cpu = smp_get_id();
  c = &s->cpu_slab[cpu];
  tid = c->tid;

  void *head = c->freelist;
  if (!head) {
    preempt_enable();
    goto slowpath;
  }

  /* Check if we have enough objects in the lockless freelist */
  void *curr = head;
  void *next;
  size_t i;
  for (i = 0; i < size - 1; i++) {
    next = get_freelist_next(curr, s->offset);
    if (!next)
      break; /* Not enough objects */
    curr = next;
  }

  if (i == size - 1) {
    /* We have enough objects. 'curr' is the last object we take. */
    /* 'next' of 'curr' becomes the new head. */
    void *new_head = get_freelist_next(curr, s->offset);

    if (likely(cmpxchg16b_local(c, head, tid, new_head, next_tid(tid)))) {
      /* Success! Fill the array */
      curr = head;
      for (size_t j = 0; j < size; j++) {
        p[j] = curr;
        if (s->flags & SLAB_POISON)
          check_poison(s, curr);
        check_redzone(s, curr);
        
        void *next_list = get_freelist_next(curr, s->offset);

        if (flags & __GFP_ZERO) {
          memset(curr, 0, s->object_size);
        }

        curr = next_list;
      }
      atomic_long_add(size, &s->total_objects);
      atomic_long_add(size, &s->alloc_fastpath);
      preempt_enable();
      return (int)size;
    }
  }

  preempt_enable();

slowpath:;
  /* Fallback to loop */
  size_t count = 0;
  for (i = 0; i < size; i++) {
    void *obj = kmem_cache_alloc(s);
    if (!obj)
      break;

    if (flags & __GFP_ZERO) {
      memset(obj, 0, s->object_size);
    }

    p[i] = obj;
    count++;
  }
  return (int)count;
}

void kmem_cache_free_bulk(kmem_cache_t *s, size_t size, void **p) {
  for (size_t i = 0; i < size; i++) {
    if (p[i])
      kmem_cache_free(s, p[i]);
  }
}

static void init_kmem_cache_node(struct kmem_cache_node *n) {
  spinlock_init(&n->list_lock);
  n->nr_partial = 0;
  INIT_LIST_HEAD(&n->partial);

  /* Initialize NUMA statistics */
  atomic_long_set(&n->alloc_hits, 0);
  atomic_long_set(&n->alloc_misses, 0);
  atomic_long_set(&n->alloc_from_partial, 0);
  atomic_long_set(&n->alloc_refills, 0);

  /* Per-node tuning - can be adjusted based on node memory */
  n->min_partial = 5;
  n->max_partial = 30;
}

static void init_kmem_cache_cpu(struct kmem_cache_cpu *c) {
  c->freelist = nullptr;
  c->page = nullptr;
  c->tid = 0;
}

#define ALIGNED_MAGIC 0xDEADBEEFCAFEBABE

/* NUMA helpers */
extern int numa_distance_get(int from, int to);

extern int numa_mem_id(void);

static void setup_numa_fallback(kmem_cache_t *s) {
  for (int node = 0; node < MAX_NUMNODES; node++) {
    if (!s->node[node])
      continue;

    /* Allocate fallback list for this node */
    int *fallback = kmalloc(sizeof(int) * MAX_NUMNODES);
    if (!fallback) {
      s->node_fallback[node] = nullptr;
      continue;
    }

    typedef struct {
      int node;
      int distance;
    } numa_candidate_t;
    numa_candidate_t candidates[MAX_NUMNODES];
    int count = 0;

    /* Populate candidates */
    for (int i = 0; i < MAX_NUMNODES; i++) {
      if (i == node || !s->node[i])
        continue;
      candidates[count].node = i;
      candidates[count].distance = numa_distance_get(node, i);
      count++;
    }

    /* Sort by distance */
    for (int i = 0; i < count - 1; i++) {
      for (int j = 0; j < count - i - 1; j++) {
        if (candidates[j].distance > candidates[j + 1].distance) {
          numa_candidate_t tmp = candidates[j];
          candidates[j] = candidates[j + 1];
          candidates[j + 1] = tmp;
        }
      }
    }

    /* Store in fallback list, terminated by -1 */
    for (int i = 0; i < count; i++) {
      fallback[i] = candidates[i].node;
    }
    if (count < MAX_NUMNODES)
      fallback[count] = -1;

    s->node_fallback[node] = fallback;
  }
}

void *kmalloc_aligned(size_t size, size_t align) {
  if (align <= 8)
    return kmalloc(size);

  // For larger alignments, allocate extra space for alignment, magic and
  // original pointer
  size_t total_size = size + align - 1 + 2 * sizeof(void *);
  void *raw = kmalloc(total_size);
  if (!raw)
    return nullptr;

  // Calculate aligned address, ensuring at least 16 bytes for metadata
  uintptr_t aligned = (uintptr_t)raw + 2 * sizeof(void *);
  aligned = (aligned + align - 1) & ~(align - 1);

  // Store magic and original pointer before aligned address
  void **magic_ptr = (void **)(aligned - 2 * sizeof(void *));
  *magic_ptr = (void *)ALIGNED_MAGIC;
  void **orig_ptr = (void **)(aligned - sizeof(void *));
  *orig_ptr = raw;

  return (void *)aligned;
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
                                unsigned long flags) {
  kmem_cache_t *s;

  spinlock_lock(&slab_lock);
  s = find_mergeable(size, align, flags);
  if (s) {
    spinlock_unlock(&slab_lock);
    return s;
  }
  spinlock_unlock(&slab_lock);

  s = kmalloc(sizeof(kmem_cache_t));
  if (!s)
    return nullptr;

  memset(s, 0, sizeof(kmem_cache_t));
  s->name = name;
  s->object_size = (int)size;
  s->align = (int)align;

#ifdef MM_HARDENING
  s->flags = flags;
#else
  s->flags = flags & ~(SLAB_POISON | SLAB_RED_ZONE);
#endif

  /* Calculate size with alignment and meta-data */
  if (align < 8)
    align = 8;
  size = (size + align - 1) & ~(align - 1);
  s->inuse = (int)size;

  if (flags & SLAB_RED_ZONE) {
    size += sizeof(uint64_t); /* Redzone */
  }

  if (size < sizeof(void *))
    size = sizeof(void *);
  s->size = (int)size;
  s->offset = 0;

  /* Determine order - aim for at least 8 objects per slab if possible,
   * but don't exceed order 5 (128KB) unless the object itself is larger. */
  size_t min_order = 0;
  while ((PAGE_SIZE << min_order) < (size_t)size) {
    min_order++;
  }

  s->order = min_order;
  while ((PAGE_SIZE << s->order) < (size_t)size * 8 && s->order < 5 &&
         s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = CONFIG_SLAB_MIN_PARTIAL;

  /*
   * Allocate per-CPU slabs - ensure CACHE_LINE_SIZE alignment.
   * Note: We use a manual array for now as we lack alloc_percpu().
   */
  s->cpu_slab = kmalloc_aligned(sizeof(struct kmem_cache_cpu) * MAX_CPUS,
                                CACHE_LINE_SIZE);
  if (!s->cpu_slab) {
    kfree(s);
    return nullptr;
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

    if (!s->node[i])
      continue;
    init_kmem_cache_node(s->node[i]);
  }

  setup_numa_fallback(s);

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

static struct kmem_cache *create_boot_cache(const char *name, size_t size,
                                            unsigned long flags) {
  if (static_idx >= BOOT_CACHES_MAX) {
    panic("SLUB: Out of boot caches (%d used).", static_idx);
  }

  kmem_cache_t *s = &static_caches[static_idx];
  memset(s, 0, sizeof(kmem_cache_t));

  s->name = name;
  s->object_size = (int)size;
  s->size = (int)size;

  /* Enforce alignment: 8 bytes for small, 16 bytes for >= 16 to support
   * cmpxchg16b */
  if (size < 8)
    s->align = 8;
  else if (size < 16)
    s->align = 8;
  else
    s->align = 16;

  s->flags = flags;
  s->inuse = (int)size;

  s->order = 0;
  while ((PAGE_SIZE << s->order) < (size_t)size * 4 &&
         s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = CONFIG_SLAB_MIN_PARTIAL;
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

void slab_verify_all(void) {
  kmem_cache_t *s;
  irq_flags_t flags = spinlock_lock_irqsave(&slab_lock);

  list_for_each_entry(s, &slab_caches, list) {
    for (int nid = 0; nid < MAX_NUMNODES; nid++) {
      struct kmem_cache_node *n = s->node[nid];
      if (!n) continue;

      spinlock_lock(&n->list_lock);
      struct page *page;
      list_for_each_entry(page, &n->partial, list) {
        if (unlikely(page->slab_cache != s)) {
           panic("SLUB: Slab cache mismatch in partial list for %s\n", s->name);
        }
        if (unlikely(page->inuse >= page->objects)) {
           panic("SLUB: Invalid inuse count in %s\n", s->name);
        }
      }
      spinlock_unlock(&n->list_lock);
    }
  }

  spinlock_unlock_irqrestore(&slab_lock, flags);
}

int slab_init(void) {
  struct crypto_tfm *tfm = crypto_alloc_tfm("hw_rng", CRYPTO_ALG_TYPE_RNG);
  if (!tfm) tfm = crypto_alloc_tfm("sw_rng", CRYPTO_ALG_TYPE_RNG);

  if (tfm) {
    crypto_rng_generate(tfm, (uint8_t *)&slab_secret, sizeof(slab_secret));
    crypto_free_tfm(tfm);
  } else {
    slab_secret = 0xdeadbeef12345678ULL;
  }

  char *names[] = {"kmalloc-8",   "kmalloc-16",  "kmalloc-32",  "kmalloc-64",
                   "kmalloc-128", "kmalloc-256", "kmalloc-512", "kmalloc-1k",
                   "kmalloc-2k",  "kmalloc-4k",  "kmalloc-8k",  "kmalloc-16k",
                   "kmalloc-32k", "kmalloc-64k", "kmalloc-128k"};

  INIT_LIST_HEAD(&slab_caches);

  for (int i = 0; i < 15; i++) {
    size_t size = 8 << i;
    unsigned long flags = SLAB_HWCACHE_ALIGN;
    kmalloc_caches[i] = create_boot_cache(names[i], size, flags);
  }

  printk(SLAB_CLASS "SLUB Hybrid initialized (%d caches, Magazine size %d)\n",
         15, SLAB_MAG_SIZE);
  return 0;
}

void *kmalloc_node(size_t size, int node) {
  if (unlikely(size > SLAB_MAX_SIZE))
    return vmalloc(size);

  int idx = kmalloc_index(size);
  if (unlikely(idx < 0))
    return nullptr;

  return kmem_cache_alloc_node(kmalloc_caches[idx], node);
}
EXPORT_SYMBOL(kmalloc_node);

void *kmalloc(size_t size) { return kmalloc_node(size, -1); }

void *kzalloc_node(size_t size, int node) {
  void *ptr = kmalloc_node(size, node);
  if (ptr) {
    memset(ptr, 0, size);
  }
  return ptr;
}
EXPORT_SYMBOL(kzalloc_node);

void *kzalloc(size_t size) { return kzalloc_node(size, -1); }
EXPORT_SYMBOL(kzalloc);

void kfree(void *ptr) {
  struct page *page;
  kmem_cache_t *s;

  if (unlikely(!ptr))
    return;

  if ((uintptr_t)ptr >= VMALLOC_VIRT_BASE &&
      (uintptr_t)ptr < VMALLOC_VIRT_END) {
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
   * Aligned allocations (from kmalloc_aligned) have a magic value and original
   * pointer.
   *
   * FIX: Check if ptr is at the start of the slab allocation.
   * Aligned pointers always have a header before them, so they cannot be
   * at the very beginning of the slab. This check prevents accessing invalid
   * memory at (ptr - 16) when ptr is at a page boundary.
   */
  if (ptr != page_address(page)) {
    void *magic = *((void **)((uintptr_t)ptr - 2 * sizeof(void *)));
    if (unlikely(magic == (void *)ALIGNED_MAGIC)) {
      void *raw = *((void **)((uintptr_t)ptr - sizeof(void *)));
      /* Ensure the raw pointer is actually within the same page or valid */
      if (raw && virt_to_head_page(raw) == page) {
        kfree(raw);
        return;
      }
    }
  }

  kmem_cache_free(s, ptr);
}

void slab_test(void) {
  uint64_t start = get_time_ns();
  printk(KERN_DEBUG SLAB_CLASS "Starting SLUB Stress Test...\n");

  /* Test 1: Basic kmalloc/kfree */
  void *ptr = kmalloc(32);
  if (!ptr)
    panic(SLAB_CLASS "kmalloc(32) failed");
  memset(ptr, 0xAA, 32);
  kfree(ptr);
  printk(KERN_DEBUG SLAB_CLASS "  - Basic Alloc/Free: OK\n");

  /* Test 2: Array Allocation (Stress) */
#define TEST_COUNT 1024
  void **ptrs = kmalloc(TEST_COUNT * sizeof(void *));
  if (!ptrs)
    panic(SLAB_CLASS "failed to allocate pointer array");

  for (int i = 0; i < TEST_COUNT; i++) {
    ptrs[i] = kmalloc(64);
    if (!ptrs[i])
      panic(SLAB_CLASS "stress alloc failed");
    memset(ptrs[i], (i & 0xFF), 64);
  }

  /* Verify patterns */
  for (int i = 0; i < TEST_COUNT; i++) {
    uint8_t val = (i & 0xFF);
    uint8_t *byte_ptr = (uint8_t *)ptrs[i];
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
  if (!large)
    panic(SLAB_CLASS "large alloc failed");
  memset(large, 0xBB, 128 * 1024);
  kfree(large);
  printk(KERN_DEBUG SLAB_CLASS "  - Large Alloc (128KB): OK\n");

  /* Test 4: Custom Cache with Hardening */
  kmem_cache_t *custom =
      kmem_cache_create("test-cache", 150, 8, SLAB_POISON | SLAB_RED_ZONE);
  if (!custom)
    panic(SLAB_CLASS "cache create failed");

  void *obj1 = kmem_cache_alloc(custom);
  void *obj2 = kmem_cache_alloc(custom);
  if (!obj1 || !obj2)
    panic(SLAB_CLASS "cache alloc failed");

  if (obj1 == obj2)
    panic(SLAB_CLASS "duplicate object returned!");

  kmem_cache_free(custom, obj1);
  kmem_cache_free(custom, obj2);
  printk(KERN_DEBUG SLAB_CLASS "  - Custom Cache with Hardening: OK\n");

  /* Test 5: Aligned Allocations */
  void *a1 = kmalloc_aligned(16, 64);
  if (!a1)
    panic(SLAB_CLASS "aligned alloc 1 failed");
  if ((uintptr_t)a1 & 63)
    panic(SLAB_CLASS "aligned alloc not 64-byte aligned!");
  kfree(a1);

  void *a2 = kmalloc_aligned(1024, 4096);
  if (!a2)
    panic(SLAB_CLASS "aligned alloc 2 failed");
  if ((uintptr_t)a2 & 4095)
    panic(SLAB_CLASS "aligned alloc not 4096-byte aligned!");
  kfree(a2);
  printk(KERN_DEBUG SLAB_CLASS "  - Aligned Allocations: OK\n");

  /* Test 6: NUMA-aware Allocations */
  for (int i = 0; i < MAX_NUMNODES; i++) {
    if (!node_data[i])
      continue;
    void *n_ptr = kmalloc_node(128, i);
    if (!n_ptr)
      panic(SLAB_CLASS "kmalloc_node failed");
    struct page *p = virt_to_head_page(n_ptr);
    if (p->node != (uint32_t)i) {
      printk(KERN_WARNING SLAB_CLASS
             "Object not on requested node %d (on %d)\n",
             i, p->node);
    }
    kfree(n_ptr);
  }
  printk(KERN_DEBUG SLAB_CLASS "  - NUMA-aware Allocations: OK\n");

  /* Test 7: Bulk Allocation */
  void *bulk[16];
  int n = kmem_cache_alloc_bulk(custom, GFP_KERNEL, 16, bulk);
  if (n != 16)
    panic(SLAB_CLASS "bulk alloc failed");
  kmem_cache_free_bulk(custom, 16, bulk);
  printk(KERN_DEBUG SLAB_CLASS "  - Bulk Alloc/Free: OK\n");

  printk(KERN_DEBUG SLAB_CLASS "SLUB Stress Test Passed. (%lld cycles)\n",
         get_time_ns() - start);
}

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(kmem_cache_alloc);
EXPORT_SYMBOL(kmem_cache_free);
EXPORT_SYMBOL(kmem_cache_create);
EXPORT_SYMBOL(slab_test);

/**
 * krealloc - Reallocate memory with new size
 * @ptr: Pointer to previously allocated memory (or nullptr)
 * @new_size: New size in bytes
 * @flags: GFP allocation flags
 *
 * Returns: Pointer to reallocated memory, or nullptr on failure
 *
 * If ptr is nullptr, behaves like kmalloc().
 * If new_size is 0, frees ptr and returns nullptr.
 * Otherwise, allocates new memory, copies old data, and frees old memory.
 */
void *krealloc(void *ptr, size_t new_size, gfp_t flags) {
  void *new_ptr;
  struct page *page;
  kmem_cache_t *cache;
  size_t old_size;

  /* nullptr ptr: behave like kmalloc */
  if (!ptr)
    return kmalloc(new_size);

  /* Zero size: free and return nullptr */
  if (new_size == 0) {
    kfree(ptr);
    return nullptr;
  }

  /* Get old object size */
  page = virt_to_head_page(ptr);
  if (!PageSlab(page)) {
    /* Not a slab object, can't determine size - allocate new and warn */
    printk(KERN_WARNING SLAB_CLASS "krealloc: ptr %p not from slab allocator\n",
           ptr);
    new_ptr = kmalloc(new_size);
    if (new_ptr) {
      /* Copy what we can (assume at least one page) */
      memcpy(new_ptr, ptr, new_size < PAGE_SIZE ? new_size : PAGE_SIZE);
    }
    return new_ptr;
  }

  cache = page->slab_cache;
  old_size = cache->object_size;

  /* If new size fits in same cache, return same pointer */
  int new_idx = kmalloc_index(new_size);
  int old_idx = kmalloc_index(old_size);

  if (new_idx == old_idx && new_idx >= 0) {
    return ptr; /* Same size class, reuse */
  }

  /* Allocate new memory */
  new_ptr = kmalloc(new_size);
  if (!new_ptr)
    return nullptr;

  /* Copy old data (use smaller of old/new size) */
  memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);

  /* Free old memory */
  kfree(ptr);

  return new_ptr;
}

/**
 * ksize - Get the actual allocated size of an object
 * @ptr: Pointer to allocated memory
 *
 * Returns: Size of the allocated object in bytes
 */
size_t ksize(const void *ptr) {
  struct page *page;
  kmem_cache_t *cache;

  if (!ptr)
    return 0;

  /* Check if it's a vmalloc allocation */
  if ((uintptr_t)ptr >= VMALLOC_VIRT_BASE &&
      (uintptr_t)ptr < VMALLOC_VIRT_END) {
    /* vmalloc doesn't track size easily, return 0 */
    return 0;
  }

  page = virt_to_head_page(ptr);
  if (!PageSlab(page))
    return 0;

  cache = page->slab_cache;
  return cache->object_size;
}

EXPORT_SYMBOL(krealloc);
EXPORT_SYMBOL(ksize);
EXPORT_SYMBOL(kmem_cache_prefill_sheaf);
EXPORT_SYMBOL(kmem_cache_alloc_from_sheaf);
EXPORT_SYMBOL(kmem_cache_refill_sheaf);
EXPORT_SYMBOL(kmem_cache_return_sheaf);
EXPORT_SYMBOL(kmem_cache_free_bulk);

/**
 * slab_numa_stats - Print NUMA statistics for all caches
 *
 * Displays per-node allocation statistics for debugging and profiling.
 */
void slab_numa_stats(void) {
  kmem_cache_t *s;

  printk(KERN_INFO SLAB_CLASS "SLUB NUMA Statistics\n");

  list_for_each_entry(s, &slab_caches, list) {
    printk(KERN_INFO SLAB_CLASS "Cache: %s (object_size=%d)\n", s->name,
           s->object_size);

    for (int nid = 0; nid < MAX_NUMNODES; nid++) {
      struct kmem_cache_node *n = s->node[nid];
      long hits = atomic_long_read(&n->alloc_hits);
      long misses = atomic_long_read(&n->alloc_misses);
      long from_partial = atomic_long_read(&n->alloc_from_partial);
      long refills = atomic_long_read(&n->alloc_refills);

      if (hits + misses == 0)
        continue; /* Skip nodes with no activity */

      long total = hits + misses;
      int hit_rate = total > 0 ? (int)((hits * 100) / total) : 0;

      printk(KERN_INFO SLAB_CLASS
             "  Node %d: hits=%ld misses=%ld (hit_rate=%d%%) partial=%ld "
             "refills=%ld nr_partial=%lu\n",
             nid, hits, misses, hit_rate, from_partial, refills, n->nr_partial);
    }
  }

  printk(KERN_INFO SLAB_CLASS "=== End NUMA Statistics ===\n");
}

EXPORT_SYMBOL(slab_numa_stats);
