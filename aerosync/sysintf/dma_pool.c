/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/dma_pool.c
 * @brief DMA pool allocator for small coherent allocations
 * @copyright (C) 2025-2026 assembler-0
 *
 * Inspired by Linux's dma_pool and FreeBSD's busdma
 */

#include <aerosync/sysintf/dma_pool.h>
#include <aerosync/sysintf/dma.h>
#include <aerosync/spinlock.h>
#include <aerosync/classes.h>
#include <aerosync/export.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <linux/list.h>
#include <mm/gfp.h>
#include <mm/slub.h>

#define POOL_POISON_FREED    0xa7
#define POOL_POISON_ALLOCATED 0xa9

struct dma_pool {
  struct list_head pools;
  const char *name;
  size_t size;
  size_t align;
  size_t boundary;
  struct list_head page_list;
  spinlock_t lock;
#ifdef CONFIG_DMA_POOL_STATS
  atomic_long_t alloc_count;
  atomic_long_t free_count;
  atomic_long_t peak_usage;
  atomic_long_t current_usage;
#endif
};

struct dma_page {
  struct list_head page_list;
  void *vaddr;
  dma_addr_t dma;
  unsigned int in_use;
  unsigned int offset;
};

static LIST_HEAD(pool_list_head);
static spinlock_t pool_list_lock = SPINLOCK_INIT;

/**
 * dma_pool_create - Create a DMA pool
 * @name: Pool name for debugging
 * @size: Size of each allocation
 * @align: Alignment requirement (must be power of 2)
 * @boundary: Boundary constraint (0 = no constraint)
 *
 * Returns: Pool handle or nullptr on failure
 */
struct dma_pool *dma_pool_create(const char *name, size_t size, size_t align, size_t boundary) {
  if (!name || size == 0 || (align & (align - 1)) != 0) {
    printk(KERN_ERR DMA_CLASS "Invalid dma_pool parameters\n");
    return nullptr;
  }

  if (align < sizeof(void *))
    align = sizeof(void *);

  if (size < align)
    size = align;

  struct dma_pool *pool = kmalloc(sizeof(*pool));
  if (!pool)
    return nullptr;

  pool->name = name;
  pool->size = size;
  pool->align = align;
  pool->boundary = boundary;
  INIT_LIST_HEAD(&pool->page_list);
  spinlock_init(&pool->lock);

#ifdef CONFIG_DMA_POOL_STATS
  atomic_long_set(&pool->alloc_count, 0);
  atomic_long_set(&pool->free_count, 0);
  atomic_long_set(&pool->peak_usage, 0);
  atomic_long_set(&pool->current_usage, 0);
#endif

  spinlock_lock(&pool_list_lock);
  list_add(&pool->pools, &pool_list_head);
  spinlock_unlock(&pool_list_lock);

  printk(KERN_DEBUG DMA_CLASS "Created DMA pool '%s' (size=%zu, align=%zu)\n",
         name, size, align);

  return pool;
}

/**
 * pool_alloc_page - Allocate a new page for the pool
 */
static struct dma_page *pool_alloc_page(struct dma_pool *pool, gfp_t gfp) {
  struct dma_page *page = kmalloc(sizeof(*page));
  if (!page)
    return nullptr;

  page->vaddr = dma_alloc_coherent(PAGE_SIZE, &page->dma, gfp | GFP_DMA32);
  if (!page->vaddr) {
    kfree(page);
    return nullptr;
  }

  INIT_LIST_HEAD(&page->page_list);
  page->in_use = 0;
  page->offset = 0;

#ifdef CONFIG_DMA_POOL_DEBUG
  memset(page->vaddr, POOL_POISON_FREED, PAGE_SIZE);
#endif

  return page;
}

/**
 * dma_pool_alloc - Allocate from DMA pool
 * @pool: Pool to allocate from
 * @gfp: Allocation flags
 * @dma_handle: Output DMA address
 *
 * Returns: Virtual address or nullptr on failure
 */
void *dma_pool_alloc(struct dma_pool *pool, gfp_t gfp, dma_addr_t *dma_handle) {
  if (!pool || !dma_handle)
    return nullptr;

  unsigned long flags = spinlock_lock_irqsave(&pool->lock);

  struct dma_page *page;
  list_for_each_entry(page, &pool->page_list, page_list) {
    if (page->offset + pool->size <= PAGE_SIZE) {
      /* Check boundary constraint */
      if (pool->boundary) {
        dma_addr_t start = page->dma + page->offset;
        dma_addr_t end = start + pool->size - 1;
        if ((start ^ end) & ~(pool->boundary - 1))
          continue;
      }

      void *ret = (char *)page->vaddr + page->offset;
      *dma_handle = page->dma + page->offset;
      page->offset += pool->size;
      page->in_use++;

#ifdef CONFIG_DMA_POOL_DEBUG
      memset(ret, POOL_POISON_ALLOCATED, pool->size);
#endif

#ifdef CONFIG_DMA_POOL_STATS
      atomic_long_inc(&pool->alloc_count);
      long usage = atomic_long_inc_return(&pool->current_usage);
      long peak = atomic_long_read(&pool->peak_usage);
      if (usage > peak)
        atomic_long_set(&pool->peak_usage, usage);
#endif

      spinlock_unlock_irqrestore(&pool->lock, flags);
      return ret;
    }
  }

  /* Need new page */
  spinlock_unlock_irqrestore(&pool->lock, flags);

  page = pool_alloc_page(pool, gfp);
  if (!page)
    return nullptr;

  flags = spinlock_lock_irqsave(&pool->lock);
  list_add(&page->page_list, &pool->page_list);

  void *ret = page->vaddr;
  *dma_handle = page->dma;
  page->offset = pool->size;
  page->in_use = 1;

#ifdef CONFIG_DMA_POOL_DEBUG
  memset(ret, POOL_POISON_ALLOCATED, pool->size);
#endif

#ifdef CONFIG_DMA_POOL_STATS
  atomic_long_inc(&pool->alloc_count);
  long usage = atomic_long_inc_return(&pool->current_usage);
  long peak = atomic_long_read(&pool->peak_usage);
  if (usage > peak)
    atomic_long_set(&pool->peak_usage, usage);
#endif

  spinlock_unlock_irqrestore(&pool->lock, flags);
  return ret;
}

/**
 * dma_pool_free - Free allocation back to pool
 * @pool: Pool to free to
 * @vaddr: Virtual address
 * @dma: DMA address
 */
void dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t dma) {
  if (!pool || !vaddr)
    return;

  unsigned long flags = spinlock_lock_irqsave(&pool->lock);

  struct dma_page *page;
  list_for_each_entry(page, &pool->page_list, page_list) {
    if (vaddr >= page->vaddr && vaddr < page->vaddr + PAGE_SIZE) {
#ifdef CONFIG_DMA_POOL_DEBUG
      /* Check for double-free */
      char *check = (char *)vaddr;
      bool is_freed = true;
      for (size_t i = 0; i < pool->size; i++) {
        if (check[i] != POOL_POISON_FREED) {
          is_freed = false;
          break;
        }
      }
      if (is_freed) {
        printk(KERN_ERR DMA_CLASS "Double free detected in pool '%s'\n", pool->name);
        spinlock_unlock_irqrestore(&pool->lock, flags);
        return;
      }
      memset(vaddr, POOL_POISON_FREED, pool->size);
#endif

      page->in_use--;

#ifdef CONFIG_DMA_POOL_STATS
      atomic_long_inc(&pool->free_count);
      atomic_long_dec(&pool->current_usage);
#endif

      /* Free page if completely unused */
      if (page->in_use == 0 && page->offset >= PAGE_SIZE) {
        list_del(&page->page_list);
        spinlock_unlock_irqrestore(&pool->lock, flags);
        dma_free_coherent(PAGE_SIZE, page->vaddr, page->dma);
        kfree(page);
        return;
      }

      spinlock_unlock_irqrestore(&pool->lock, flags);
      return;
    }
  }

  spinlock_unlock_irqrestore(&pool->lock, flags);
  printk(KERN_WARNING DMA_CLASS "dma_pool_free: invalid address %p\n", vaddr);
}

/**
 * dma_pool_destroy - Destroy a DMA pool
 * @pool: Pool to destroy
 */
void dma_pool_destroy(struct dma_pool *pool) {
  if (!pool)
    return;

  spinlock_lock(&pool_list_lock);
  list_del(&pool->pools);
  spinlock_unlock(&pool_list_lock);

  struct dma_page *page, *tmp;
  list_for_each_entry_safe(page, tmp, &pool->page_list, page_list) {
    if (page->in_use > 0) {
      printk(KERN_WARNING DMA_CLASS "Pool '%s' destroyed with %u allocations in use\n",
             pool->name, page->in_use);
    }
    list_del(&page->page_list);
    dma_free_coherent(PAGE_SIZE, page->vaddr, page->dma);
    kfree(page);
  }

#ifdef CONFIG_DMA_POOL_STATS
  printk(KERN_DEBUG DMA_CLASS "Pool '%s' stats: alloc=%ld free=%ld peak=%ld\n",
         pool->name,
         atomic_long_read(&pool->alloc_count),
         atomic_long_read(&pool->free_count),
         atomic_long_read(&pool->peak_usage));
#endif

  kfree(pool);
}

EXPORT_SYMBOL(dma_pool_create);
EXPORT_SYMBOL(dma_pool_alloc);
EXPORT_SYMBOL(dma_pool_free);
EXPORT_SYMBOL(dma_pool_destroy);
