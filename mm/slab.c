///SPDX-License-Identifier: GPL-2.0-only
/**
 * VoidFrameX monolithic kernel
 *
 * @file mm/slab.c
 * @brief Advanced SLUB allocator implementation
 * @copyright (C) 2025 assembler-0
*
 * This file is part of the VoidFrameX kernel.
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
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/layout.h>
#include <arch/x64/smp.h>
#include <linux/container_of.h>
#include <kernel/fkx/fkx.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/panic.h>
#include <kernel/classes.h>

static LIST_HEAD(slab_caches);
static spinlock_t slab_lock = 0;

static kmem_cache_t *kmalloc_caches[15];

/* Helper to get the next object in the freelist */
static inline void *get_freelist_next(void *obj, int offset) {
  return *(void **) ((char *) obj + offset);
}

static inline void set_freelist_next(void *obj, int offset, void *next) {
  *(void **) ((char *) obj + offset) = next;
}

static struct page *allocate_slab(kmem_cache_t *s, gfp_t flags, int node) {
  struct page *page;
  void *start;
  void *p;

  page = alloc_pages(flags, s->order);
  if (!page)
    return NULL;

  start = page_address(page);
  page->slab_cache = s;
  page->objects = (PAGE_SIZE << s->order) / s->size;
  page->inuse = 0;
  page->frozen = 0;
  SetPageSlab(page);
  INIT_LIST_HEAD(&page->list);

  /* Build freelist */
  page->freelist = start;
  for (int i = 0; i < (int) page->objects - 1; i++) {
    p = (char *) start + i * s->size;
    set_freelist_next(p, s->offset, (char *) p + s->size);
  }
  set_freelist_next((char *) start + (page->objects - 1) * s->size, s->offset, NULL);

  return page;
}

static void __free_slab(kmem_cache_t *s, struct page *page) {
  ClearPageSlab(page);
  __free_pages(page, s->order);
}

/* Slowpath allocation */
static void *__slab_alloc(kmem_cache_t *s, gfp_t gfpflags, int node, struct kmem_cache_cpu *c) {
  void *freelist;
  struct page *page;
  irq_flags_t flags;

  flags = save_irq_flags();

  /* Check if we have a page but no freelist (slab frozen) */
  page = c ? c->page : NULL;
  if (!page)
    goto new_slab;

  /* Try to get objects from the frozen page's freelist */
  freelist = page->freelist;
  if (freelist) {
    page->freelist = NULL; /* Take the whole freelist */
    page->inuse = (unsigned short) page->objects;
    if (c) c->freelist = get_freelist_next(freelist, s->offset);
    else {
        // If no CPU cache, we must put the rest of the freelist back to the page
        // But the whole point of freezing is to take the whole list.
        // For NULL c, we just take ONE object and keep the rest in page->freelist.
        page->freelist = get_freelist_next(freelist, s->offset);
        page->inuse = (unsigned short)(page->objects - 1); // wait, inuse is total objects if frozen.
        // Actually, for NULL c, we don't freeze.
    }
    restore_irq_flags(flags);
    return freelist;
  }

  /* Page is full, unfreeze it */
  page->frozen = 0;
  if (c) c->page = NULL;

new_slab:
  /* Check partial list */
  spinlock_lock(&s->node.list_lock);
  if (!list_empty(&s->node.partial)) {
    page = list_first_entry(&s->node.partial, struct page, list);
    list_del(&page->list);
    s->node.nr_partial--;
    spinlock_unlock(&s->node.list_lock);

    if (c) {
        /* Freeze this page to the CPU */
        page->frozen = 1;
        c->page = page;
        freelist = page->freelist;
        page->freelist = NULL;
        page->inuse = (unsigned short) page->objects;
        c->freelist = get_freelist_next(freelist, s->offset);
    } else {
        /* Just take one object and put back to partial if not empty */
        freelist = page->freelist;
        page->freelist = get_freelist_next(freelist, s->offset);
        page->inuse++;
        if (page->freelist) {
            spinlock_lock(&s->node.list_lock);
            list_add(&page->list, &s->node.partial);
            s->node.nr_partial++;
            spinlock_unlock(&s->node.list_lock);
        }
    }
    restore_irq_flags(flags);
    return freelist;
  }
  spinlock_unlock(&s->node.list_lock);

  /* Allocate new slab */
  page = allocate_slab(s, gfpflags, node);
  if (!page) {
    restore_irq_flags(flags);
    printk(KERN_ERR SLAB_CLASS "Failed to allocate new slab for %s\n", s->name);
    return NULL;
  }

  if (c) {
      page->frozen = 1;
      c->page = page;
      freelist = page->freelist;
      page->freelist = NULL;
      page->inuse = (unsigned short) page->objects;
      c->freelist = get_freelist_next(freelist, s->offset);
  } else {
      freelist = page->freelist;
      page->freelist = get_freelist_next(freelist, s->offset);
      page->inuse++;
      // New slab is partial if it has more than 1 object
      if (page->freelist) {
          spinlock_lock(&s->node.list_lock);
          list_add(&page->list, &s->node.partial);
          s->node.nr_partial++;
          spinlock_unlock(&s->node.list_lock);
      }
  }

  restore_irq_flags(flags);
  return freelist;
}

void *kmem_cache_alloc(kmem_cache_t *s) {
  irq_flags_t flags;
  void *object;
  struct kmem_cache_cpu *c;
  int cpu = smp_is_active() ? (int) smp_get_id() : 0;

  if (unlikely(cpu >= MAX_CPUS)) {
    return __slab_alloc(s, GFP_KERNEL, -1, NULL);
  }

  flags = save_irq_flags();
  c = &s->cpu_slab[cpu];
  object = c->freelist;

  if (unlikely(!object)) {
    object = __slab_alloc(s, GFP_KERNEL, -1, c);
  } else {
    c->freelist = get_freelist_next(object, s->offset);
    c->tid++;
  }

  restore_irq_flags(flags);
  return object;
}

static void __slab_free(kmem_cache_t *s, struct page *page, void *x) {
  irq_flags_t flags;
  void *prior;
  int was_frozen;

  flags = spinlock_lock_irqsave(&s->node.list_lock);

  prior = page->freelist;
  set_freelist_next(x, s->offset, prior);
  page->freelist = x;
  page->inuse--;

  was_frozen = (int) page->frozen;

  if (unlikely(!page->inuse)) {
    /* Slab is empty, free it if we have enough partials */
    /* IMPORTANT: Never free a frozen slab, the CPU still owns it! */
    if (!was_frozen && s->node.nr_partial > s->min_partial) {
      /* If it was on partial list (had prior freelist) */
      if (prior) {
        list_del(&page->list);
        s->node.nr_partial--;
      }
      spinlock_unlock_irqrestore(&s->node.list_lock, flags);
      __free_slab(s, page);
      return;
    }
  }

  /* If it was full, it might need to go to partial list */
  if (!was_frozen && !prior) {
    list_add(&page->list, &s->node.partial);
    s->node.nr_partial++;
  }

  spinlock_unlock_irqrestore(&s->node.list_lock, flags);
}

void kmem_cache_free(kmem_cache_t *s, void *x) {
  irq_flags_t flags;
  struct page *page;
  struct kmem_cache_cpu *c;
  int cpu = smp_is_active() ? (int) smp_get_id() : 0;

  if (unlikely(!x)) return;

  page = virt_to_head_page(x);

  if (unlikely(cpu >= MAX_CPUS)) {
    __slab_free(s, page, x);
    return;
  }

  flags = save_irq_flags();
  c = &s->cpu_slab[cpu];

  if (likely(page == c->page)) {
    set_freelist_next(x, s->offset, c->freelist);
    c->freelist = x;
    c->tid++;
    restore_irq_flags(flags);
  } else {
    restore_irq_flags(flags);
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

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags) {
  kmem_cache_t *s;

  s = kmalloc(sizeof(kmem_cache_t));
  if (!s) return NULL;

  memset(s, 0, sizeof(kmem_cache_t));
  s->name = name;
  s->object_size = (int) size;
  s->align = (int) align;
  s->flags = flags;

  /* Calculate size with alignment */
  size = (size + align - 1) & ~(align - 1);
  if (size < sizeof(void *)) size = sizeof(void *);
  s->size = (int) size;
  s->offset = 0; /* Free pointer at start of object */

  /* Determine order */
  s->order = 0;
  while ((PAGE_SIZE << s->order) < size * 4 && s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = 5;

  init_kmem_cache_node(&s->node);
  for (int i = 0; i < MAX_CPUS; i++) {
    init_kmem_cache_cpu(&s->cpu_slab[i]);
  }

  spinlock_lock(&slab_lock);
  list_add(&s->list, &slab_caches);
  spinlock_unlock(&slab_lock);

  return s;
}

static struct kmem_cache *create_boot_cache(const char *name, size_t size, unsigned long flags) {
  /* Special allocation for initial caches since kmalloc isn't ready */
  /* We use a static array for the descriptors of the kmalloc caches themselves */
  static kmem_cache_t static_caches[32];
  static int static_idx = 0;

  if (static_idx >= 32) {
    panic("SLUB: Out of boot caches");
  }

  kmem_cache_t *s = &static_caches[static_idx++];
  memset(s, 0, sizeof(kmem_cache_t));

  s->name = name;
  s->object_size = (int) size;
  s->size = (int) size;
  s->align = 8;
  s->flags = flags;

  /* Determine order */
  s->order = 0;
  while ((PAGE_SIZE << s->order) < size && s->order < SLAB_MAX_ORDER) {
    s->order++;
  }

  s->min_partial = 2;

  init_kmem_cache_node(&s->node);
  for (int i = 0; i < MAX_CPUS; i++) {
    init_kmem_cache_cpu(&s->cpu_slab[i]);
  }

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
    kmalloc_caches[i] = create_boot_cache(names[i], size, 0);
  }

  printk(SLAB_CLASS "SLUB allocator initialized (%d caches)\n", sizeof(names) / sizeof(names[0]));
}

void *kmalloc(size_t size) {
  if (unlikely(size > SLAB_MAX_SIZE))
    return vmalloc(size);

  for (int i = 0; i < 15; i++) {
    if (size <= (size_t) kmalloc_caches[i]->object_size) {
      void *p = kmem_cache_alloc(kmalloc_caches[i]);
      if (!p) {
        printk(KERN_ERR SLAB_CLASS "kmalloc(%zu) failed in cache %s\n", size, kmalloc_caches[i]->name);
      }
      return p;
    }
  }

  return NULL;
}

void kfree(void *ptr) {
  struct page *page;

  if (unlikely(!ptr)) return;

  if ((uintptr_t) ptr >= VMALLOC_VIRT_BASE && (uintptr_t) ptr < VMALLOC_VIRT_END) {
    vfree(ptr);
    return;
  }

  page = virt_to_head_page(ptr);
  if (unlikely(!PageSlab(page))) {
    /* Possibly a large pmm allocation not through slub? */
    /* For now assume it must be slub or vmalloc */
    return;
  }

  kmem_cache_free(page->slab_cache, ptr);
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
    if (!ptrs[i]) panic(SLAB_CLASS "stress alloc failed at");
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

  /* Test 4: Custom Cache */
  kmem_cache_t *custom = kmem_cache_create("test-cache", 150, 8, 0);
  if (!custom) panic(SLAB_CLASS "cache create failed");

  void *obj1 = kmem_cache_alloc(custom);
  void *obj2 = kmem_cache_alloc(custom);
  if (!obj1 || !obj2) panic(SLAB_CLASS "cache alloc failed");

  if (obj1 == obj2) panic(SLAB_CLASS "duplicate object returned!");

  kmem_cache_free(custom, obj1);
  kmem_cache_free(custom, obj2);
  printk(KERN_DEBUG SLAB_CLASS "  - Custom Cache: OK\n");

  printk(KERN_DEBUG SLAB_CLASS "SLUB Stress Test Passed.\n");
}

EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
EXPORT_SYMBOL(kmem_cache_alloc);
EXPORT_SYMBOL(kmem_cache_free);
EXPORT_SYMBOL(kmem_cache_create);
EXPORT_SYMBOL(slab_test);
