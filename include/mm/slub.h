#pragma once

#include <aerosync/spinlock.h>
#include <aerosync/types.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/mm/pmm.h>
#include <mm/gfp.h>
#include <mm/page.h>

#ifndef SLAB_MAX_ORDER
#ifndef CONFIG_SLAB_MAX_ORDER
#define SLAB_MAX_ORDER 11
#else
#define SLAB_MAX_ORDER CONFIG_SLAB_MAX_ORDER
#endif
#endif

#define SLAB_MAX_SIZE (128 * 1024)

#ifndef SLAB_MAG_SIZE
#ifndef CONFIG_SLAB_MAG_SIZE
#define SLAB_MAG_SIZE 16
#else
#define SLAB_MAG_SIZE CONFIG_SLAB_MAG_SIZE
#endif
#endif

#define CACHE_LINE_SIZE 64

/* SLUB flags */
#define SLAB_POISON 0x00000800UL
#define SLAB_RED_ZONE 0x00002000UL
#define SLAB_HWCACHE_ALIGN 0x00008000UL
#define SLAB_TYPESAFE_BY_RCU 0x00080000UL /* RCU-free slabs */

alignas(CACHE_LINE_SIZE) struct kmem_cache_cpu {
  void *freelist;    /* Pointer to next available object */
  unsigned long tid; /* Transaction ID for lockless cmpxchg */
  struct page *page; /* The slab from which we are allocating */

  /* Magazine Layer (BSD/XNU Hybrid) */
  void *mag[SLAB_MAG_SIZE];
  int mag_count;
};

struct kmem_cache_node {
  spinlock_t list_lock;
  unsigned long nr_partial;
  struct list_head partial;
  atomic_long_t nr_slabs;
  atomic_long_t total_objects;

  /* NUMA-aware statistics */
  atomic_long_t alloc_hits;   /* Allocations from this node */
  atomic_long_t alloc_misses; /* Allocations that fell back to other nodes */
  atomic_long_t alloc_from_partial; /* Allocations from partial list */
  atomic_long_t alloc_refills; /* Number of times partial list was refilled */

  /* Node-specific tuning */
  unsigned long min_partial; /* Per-node minimum partial slabs */
  unsigned long max_partial; /* Per-node maximum partial slabs */
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
  int *node_fallback[MAX_NUMNODES]; /* Precomputed NUMA fallback lists */

  const char *name;
  struct list_head list; /* List of all slabs */

  /* Alignment */
  int align;

  /* Redzone and poisoning */
  int inuse; /* offset to redzone / end of object */

  /* Management Stats */
  atomic_long_t active_slabs;
  atomic_long_t total_objects;

  /* Performance Statistics (for profiling) */
  atomic_long_t alloc_fastpath;
  atomic_long_t alloc_slowpath;
  atomic_long_t free_fastpath;
  atomic_long_t free_slowpath;
} kmem_cache_t;

/* API */
int slab_init(void);

void slab_test(void);
void slab_verify_all(void);

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align,
                                unsigned long flags);

void *kmem_cache_alloc(kmem_cache_t *cache);
void *kmem_cache_alloc_node(kmem_cache_t *cache, int node);
void kmem_cache_free(kmem_cache_t *cache, void *obj);
int kmem_cache_alloc_bulk(kmem_cache_t *s, gfp_t flags, size_t size, void **p);
void kmem_cache_free_bulk(kmem_cache_t *s, size_t size, void **p);

/* Sheaf bulk allocation API - see mm/slab_sheaf.h for details */
#define SHEAF_MAX_OBJECTS 64

struct slab_sheaf {
  void **objects;           /* Array of object pointers */
  size_t capacity;          /* Maximum objects (SHEAF_MAX_OBJECTS) */
  size_t count;             /* Current number of objects */
  struct kmem_cache *cache; /* Associated cache */
  int node;                 /* NUMA node affinity */
};

struct slab_sheaf *kmem_cache_prefill_sheaf(kmem_cache_t *cache, gfp_t gfp,
                                            size_t count);
void *kmem_cache_alloc_from_sheaf(kmem_cache_t *cache, gfp_t gfp,
                                  struct slab_sheaf *sheaf);
int kmem_cache_refill_sheaf(kmem_cache_t *cache, gfp_t gfp,
                            struct slab_sheaf *sheaf, size_t count);
void kmem_cache_return_sheaf(kmem_cache_t *cache, gfp_t gfp,
                             struct slab_sheaf *sheaf);

static inline size_t kmem_cache_sheaf_size(struct slab_sheaf *sheaf) {
  return sheaf ? sheaf->count : 0;
}

void *kmalloc(size_t size);
void *kmalloc_node(size_t size, int node);
void *kmalloc_aligned(size_t size, size_t align);
void *kzalloc(size_t size);
void *kzalloc_node(size_t size, int node);
void kfree(void *ptr);

/* Helpers to convert between object and page */
static inline struct page *virt_to_head_page(const void *x) {
  return virt_to_page((void *)x);
}

/* Advanced memory management */
void *krealloc(void *ptr, size_t new_size, gfp_t flags);
size_t ksize(const void *ptr);

/**
 * kmalloc_array_node - allocate memory for an array on a specific node
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 * @node: node to allocate from.
 *
 * return: pointer to the allocated memory or %nullptr in case of error
 */
static inline void *kmalloc_array_node(size_t n, size_t size, gfp_t flags,
                                       int node) {
  size_t bytes;

  if (unlikely(__builtin_mul_overflow(n, size, &bytes)))
    return nullptr;

  if (flags & __GFP_ZERO)
    return kzalloc_node(bytes, node);

  return kmalloc_node(bytes, node);
}

/**
 * kmalloc_array - allocate memory for an array.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 *
 * return: pointer to the allocated memory or %nullptr in case of error
 */
static inline void *kmalloc_array(size_t n, size_t size, gfp_t flags) {
  return kmalloc_array_node(n, size, flags, -1);
}

/**
 * kcalloc_node - allocate memory for an array on a specific node and zero it.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 * @node: node to allocate from.
 *
 * return: pointer to the allocated memory or %nullptr in case of error
 */
static inline void *kcalloc_node(size_t n, size_t size, gfp_t flags, int node) {
  return kmalloc_array_node(n, size, flags | __GFP_ZERO, node);
}

/**
 * kcalloc - allocate memory for an array and zero it.
 * @n: number of elements.
 * @size: element size.
 * @flags: the type of memory to allocate (see kmalloc).
 *
 * return: pointer to the allocated memory or %nullptr in case of error
 */
static inline void *kcalloc(size_t n, size_t size, gfp_t flags) {
  return kmalloc_array(n, size, flags | __GFP_ZERO);
}

