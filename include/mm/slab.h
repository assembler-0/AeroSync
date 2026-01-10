#pragma once

#include <aerosync/spinlock.h>
#include <aerosync/types.h>

#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/pmm.h>
#include <aerosync/spinlock.h>
#include <aerosync/types.h>
#include <mm/page.h>

#define SLAB_MAX_ORDER 11
#define SLAB_MAX_SIZE (128 * 1024)

/* SLUB flags */
#define SLAB_POISON 0x00000800UL
#define SLAB_RED_ZONE 0x00002000UL
#define SLAB_HWCACHE_ALIGN 0x00008000UL

struct kmem_cache_cpu {
  void *freelist;    /* Pointer to next available object */
  unsigned long tid; /* Transaction ID for lockless cmpxchg */
  struct page *page; /* The slab from which we are allocating */
} __aligned(16);

struct kmem_cache_node {
  spinlock_t list_lock;
  unsigned long nr_partial;
  struct list_head partial;
#ifdef CONFIG_SLUB_DEBUG
  atomic_long_t nr_slabs;
  atomic_long_t total_objects;
#endif
};

typedef struct kmem_cache {
  struct kmem_cache_cpu __percpu *cpu_slab;

  /* Used for slowpath */
  unsigned long flags;
  unsigned long min_partial;
  int size;           /* The size of an object including meta data */
  int object_size;    /* The size of an object without meta data */
  int offset;         /* Free pointer offset. */
  unsigned int order; /* PMM allocation order */

  /* Slabs per node */
  struct kmem_cache_node *node[MAX_NUMNODES];

  const char *name;
  struct list_head list; /* List of all slabs */

  /* Alignment */
  int align;

  /* Redzone and poisoning */
  int inuse; /* offset to redzone / end of object */

  /* Management Stats */
  atomic_long_t active_slabs;
  atomic_long_t total_objects;
} kmem_cache_t;

/* API */
void slab_init(void);
void slab_test(void);
kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
                                unsigned long flags);
void *kmem_cache_alloc(kmem_cache_t *cache);
void kmem_cache_free(kmem_cache_t *cache, void *obj);

void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t align);
void *kzalloc(size_t size);
void kfree(void *ptr);

/* Helpers to convert between object and page */
static inline struct page *virt_to_head_page(const void *x) {
  return virt_to_page((void *)x);
}