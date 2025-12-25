#pragma once

#include <kernel/types.h>
#include <kernel/spinlock.h>

#define SLAB_MAX_SIZE    2048    // Objects larger than this go to PMM/VM

struct slab_page;

typedef struct kmem_cpu_cache {
  void **free_list;            // Fast-path: list of free objects
  struct slab_page *page;      // The page we are currently allocating from
} kmem_cpu_cache_t;

typedef struct kmem_cache {
  const char *name;
  size_t size;                // Object size
  size_t align;
  uint32_t objs_per_slab;

  // SMP caches
  kmem_cpu_cache_t cpu_caches[32]; // Adjust based on MAX_CPUS

  // Global list for slabs with free space (Partial)
  spinlock_t node_lock;
  struct slab_page *partial_list;

  struct kmem_cache *next;    // Global list of caches
} kmem_cache_t;

// This header resides at the very beginning of the allocated page
typedef struct slab_page {
  kmem_cache_t *cache;
  void **free_list;           // Objects free in this specific page
  uint32_t in_use;            // Number of allocated objects
  struct slab_page *next;     // Linkage for partial list
  uint32_t on_partial_list;   // Flag to track list membership
  int cpu;                    // CPU owning this slab as active, -1 if none
} slab_page_t;

// API
void slab_init(void);
kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align);
void *kmem_cache_alloc(kmem_cache_t *cache);
void kmem_cache_free(kmem_cache_t *cache, void *obj);

void *kmalloc(size_t size);
void kfree(void *ptr);