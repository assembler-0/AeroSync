#pragma once

#include <kernel/types.h>
#include <kernel/spinlock.h>

#define SLAB_MAGIC_ALLOC 0xDEADBEEF
#define SLAB_MAGIC_FREE  0xFEEDFACE
#define SLAB_GUARD_SIZE  16

// Allocation regions
typedef enum {
    ALLOC_REGION_KERNEL = 0,  // General kernel allocations
    ALLOC_REGION_STACK,       // Stack allocations (stricter)
    ALLOC_REGION_DMA,         // DMA-coherent memory
    ALLOC_REGION_COUNT
} alloc_region_t;

// Slab sizes (powers of 2)
#define SLAB_MIN_SIZE 16
#define SLAB_MAX_SIZE 2048
#define SLAB_COUNT 8  // 16, 32, 64, 128, 256, 512, 1024, 2048

typedef struct slab_obj {
    uint32_t magic;
    uint32_t size;
    struct slab_obj *next;
    alloc_region_t region;
    uint8_t guard_pre[SLAB_GUARD_SIZE];
} __attribute__((aligned(8))) slab_obj_t;

typedef struct slab_cache {
    size_t obj_size;
    size_t objs_per_page;
    struct slab_obj *free_list;
    uint64_t total_objs;
    uint64_t free_objs;
    spinlock_t lock;
} slab_cache_t;

typedef struct slab_region {
    const char *name;
    slab_cache_t caches[SLAB_COUNT];
    uint64_t total_allocated;
    uint64_t peak_allocated;
    bool strict_guards;
} slab_region_t;

// Core API
int slab_init(void);
void *kmalloc(size_t size);
void *kmalloc_region(size_t size, alloc_region_t region);
void kfree(void *ptr);

// Stack allocator (stricter)
void *alloc_stack(size_t size);
void free_stack(void *ptr);

// Debug/stats
void slab_dump_stats(void);
bool slab_check_guards(void *ptr);

// Internal
static inline size_t slab_size_to_index(size_t size);
static inline size_t slab_index_to_size(size_t index);