///SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/slab_sheaf.c
 * @brief Sheaf bulk allocation subsystem for SLUB
 * @copyright (C) 2025-2026 assembler-0
 *
 * Sheaves provide per-CPU bulk allocation caching for high-performance
 * scenarios where multiple objects need to be allocated/freed atomically.
 * Inspired by Linux 6.18+ sheaf implementation for maple trees.
 */

#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/panic.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <mm/gfp.h>
#include <mm/slub.h>

struct slab_sheaf *kmem_cache_prefill_sheaf(struct kmem_cache *cache, gfp_t gfp,
                                            size_t count) {
  struct slab_sheaf *sheaf;
  int allocated;

  if (!cache || count == 0 || count > SHEAF_MAX_OBJECTS)
    return nullptr;

  /* Allocate the sheaf structure itself */
  sheaf = (struct slab_sheaf *)kmalloc(sizeof(struct slab_sheaf));
  if (!sheaf)
    return nullptr;

  /* Allocate the object pointer array */
  sheaf->objects = (void **)kmalloc(sizeof(void *) * SHEAF_MAX_OBJECTS);
  if (!sheaf->objects) {
    kfree(sheaf);
    return nullptr;
  }

  sheaf->cache = cache;
  sheaf->capacity = SHEAF_MAX_OBJECTS;
  sheaf->count = 0;
  sheaf->node = -1; /* Will be set to local node on first alloc */

  /* Prefill with objects using bulk allocator */
  allocated = kmem_cache_alloc_bulk(cache, gfp, count, sheaf->objects);
  if (allocated <= 0) {
      kfree(sheaf->objects);
      kfree(sheaf);
      return nullptr;
  }
  
  sheaf->count = allocated;

  unmet_cond_warn(allocated < count);

  return sheaf;
}

void *kmem_cache_alloc_from_sheaf(struct kmem_cache *cache, gfp_t gfp,
                                  struct slab_sheaf *sheaf) {
  (void)gfp; /* Unused, for API compatibility */

  if (!sheaf || !cache || sheaf->cache != cache || sheaf->count == 0)
    return nullptr;

  /* Fast O(1) pop from array */
  return sheaf->objects[--sheaf->count];
}

int kmem_cache_refill_sheaf(struct kmem_cache *cache, gfp_t gfp,
                            struct slab_sheaf *sheaf, size_t count) {
  int added;

  if (!sheaf || !cache || sheaf->cache != cache)
    return -EINVAL;

  /* Calculate how many we can actually add */
  size_t space = sheaf->capacity - sheaf->count;
  if (count > space)
    count = space;
  
  if (count == 0) return 0;

  /* Allocate objects directly into the array at the correct offset */
  added = kmem_cache_alloc_bulk(cache, gfp, count, &sheaf->objects[sheaf->count]);
  if (added > 0) {
      sheaf->count += added;
  }

  return added;
}

void kmem_cache_return_sheaf(struct kmem_cache *cache, gfp_t gfp,
                             struct slab_sheaf *sheaf) {
  (void)gfp; /* Unused */

  if (!sheaf)
    return;

  unmet_cond_crit(!cache || sheaf->cache != cache);

  /* Bulk free all objects */
  kmem_cache_free_bulk(cache, sheaf->count, sheaf->objects);
  sheaf->count = 0;

  /* Free the sheaf itself */
  kfree(sheaf->objects);
  kfree(sheaf);
}