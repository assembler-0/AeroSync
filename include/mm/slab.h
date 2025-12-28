#pragma once

#include <kernel/types.h>
#include <kernel/spinlock.h>

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <arch/x64/cpu.h>
#include <mm/page.h>
#include <arch/x64/mm/pmm.h>

#define SLAB_MAX_ORDER   11
#define SLAB_MAX_SIZE    (128 * 1024)

/* SLUB flags */
#define SLAB_POISON      0x00000800UL
#define SLAB_RED_ZONE    0x00002000UL
#define SLAB_HWCACHE_ALIGN 0x00008000UL

struct kmem_cache_cpu {
    void *freelist;       /* Pointer to next available object */
    unsigned long tid;    /* Transaction ID for lockless cmpxchg */
    struct page *page;    /* The slab from which we are allocating */
};

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
    struct kmem_cache_cpu cpu_slab[MAX_CPUS];
    
    /* Used for slowpath */
    unsigned long flags;
    unsigned long min_partial;
    int size;             /* The size of an object including meta data */
    int object_size;      /* The size of an object without meta data */
    int offset;           /* Free pointer offset. */
    unsigned int order;   /* PMM allocation order */
    
    /* Slabs per node (simplified for now to single node) */
    struct kmem_cache_node node;
    
    const char *name;
    struct list_head list; /* List of all slabs */
    
    /* Alignment */
    int align;
} kmem_cache_t;

/* API */
void slab_init(void);
void slab_test(void);
kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align, unsigned long flags);
void *kmem_cache_alloc(kmem_cache_t *cache);
void kmem_cache_free(kmem_cache_t *cache, void *obj);

void *kmalloc(size_t size);
void kfree(void *ptr);

/* Helpers to convert between object and page */
static inline struct page *virt_to_head_page(const void *x) {
    return virt_to_page((void *)x);
}