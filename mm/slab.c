#include <mm/slab.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/vmm.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/panic.h>

// Virtual memory region for slab allocator
#define SLAB_VIRT_BASE 0xFFFF800000000000UL
#define SLAB_VIRT_SIZE (1UL << 30) // 1GB virtual space
static uint64_t g_slab_virt_next = SLAB_VIRT_BASE;
static spinlock_t slab_vmm_lock = 0;

static slab_region_t regions[ALLOC_REGION_COUNT];
static bool slab_initialized = false;

// Size mapping: 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
static const size_t slab_sizes[SLAB_COUNT] = {
    16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

static inline size_t slab_size_to_index(size_t size) {
    if (size <= 16) return 0;
    if (size <= 32) return 1;
    if (size <= 64) return 2;
    if (size <= 128) return 3;
    if (size <= 256) return 4;
    if (size <= 512) return 5;
    if (size <= 1024) return 6;
    if (size <= 2048) return 7;
    return 8; // 4096
}

static inline size_t slab_index_to_size(size_t index) {
    return slab_sizes[index];
}

static void slab_fill_guards(slab_obj_t *obj, bool strict) {
    uint8_t pattern = strict ? 0xAA : 0x55;
    memset(obj->guard_pre, pattern, SLAB_GUARD_SIZE);
    
    uint8_t *guard_post = (uint8_t *)obj + sizeof(slab_obj_t) + obj->size;
    memset(guard_post, pattern, SLAB_GUARD_SIZE);
}

static bool slab_check_guards_internal(slab_obj_t *obj) {
    bool strict = regions[obj->region].strict_guards;
    uint8_t pattern = strict ? 0xAA : 0x55;
    
    // Check pre-guard
    for (int i = 0; i < SLAB_GUARD_SIZE; i++) {
        if (obj->guard_pre[i] != pattern) return false;
    }
    
    // Check post-guard
    uint8_t *guard_post = (uint8_t *)obj + sizeof(slab_obj_t) + obj->size;
    for (int i = 0; i < SLAB_GUARD_SIZE; i++) {
        if (guard_post[i] != pattern) return false;
    }
    
    return true;
}

static void *slab_alloc_page(void) {
    // Allocate physical page
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    
    // Allocate virtual address
    spinlock_lock(&slab_vmm_lock);
    uint64_t virt = g_slab_virt_next;
    g_slab_virt_next += PAGE_SIZE;
    
    if (virt >= SLAB_VIRT_BASE + SLAB_VIRT_SIZE) {
        spinlock_unlock(&slab_vmm_lock);
        pmm_free_page(phys);
        return NULL; // Out of virtual space
    }
    spinlock_unlock(&slab_vmm_lock);
    
    // Map the page
    if (vmm_map_page(g_kernel_pml4, virt, phys, PTE_PRESENT | PTE_RW | PTE_NX) != 0) {
        pmm_free_page(phys);
        return NULL;
    }
    
    return (void *)virt;
}

static slab_obj_t *slab_alloc_obj(slab_cache_t *cache, alloc_region_t region) {
    if (cache->free_list) {
        slab_obj_t *obj = cache->free_list;
        cache->free_list = obj->next;
        cache->free_objs--;
        
        obj->magic = SLAB_MAGIC_ALLOC;
        obj->region = region;
        obj->next = NULL;
        slab_fill_guards(obj, regions[region].strict_guards);
        
        return obj;
    }
    
    // Allocate new page via VMM
    void *page = slab_alloc_page();
    if (!page) return NULL;
    
    size_t obj_total_size = sizeof(slab_obj_t) + cache->obj_size + SLAB_GUARD_SIZE;
    size_t objs_in_page = PAGE_SIZE / obj_total_size;
    
    // Initialize objects in page
    for (size_t i = 0; i < objs_in_page; i++) {
        slab_obj_t *obj = (slab_obj_t *)((uint8_t *)page + i * obj_total_size);
        obj->magic = SLAB_MAGIC_FREE;
        obj->size = cache->obj_size;
        obj->region = region;
        
        if (i == 0) {
            // Return first object
            obj->magic = SLAB_MAGIC_ALLOC;
            obj->next = NULL;
            slab_fill_guards(obj, regions[region].strict_guards);
            cache->total_objs++;
            
            // Add rest to free list
            if (objs_in_page > 1) {
                slab_obj_t *next_obj = (slab_obj_t *)((uint8_t *)page + obj_total_size);
                cache->free_list = next_obj;
                
                for (size_t j = 1; j < objs_in_page; j++) {
                    slab_obj_t *curr = (slab_obj_t *)((uint8_t *)page + j * obj_total_size);
                    curr->next = (j == objs_in_page - 1) ? NULL : 
                                (slab_obj_t *)((uint8_t *)page + (j + 1) * obj_total_size);
                    cache->total_objs++;
                    cache->free_objs++;
                }
            }
            
            return obj;
        }
    }
    
    return NULL;
}

int slab_init(void) {
    // Initialize regions
    regions[ALLOC_REGION_KERNEL] = (slab_region_t){
        .name = "kernel",
        .strict_guards = false
    };
    
    regions[ALLOC_REGION_STACK] = (slab_region_t){
        .name = "stack",
        .strict_guards = true
    };
    
    regions[ALLOC_REGION_DMA] = (slab_region_t){
        .name = "dma",
        .strict_guards = false
    };
    
    // Initialize caches for each region
    for (int r = 0; r < ALLOC_REGION_COUNT; r++) {
        for (int i = 0; i < SLAB_COUNT; i++) {
            slab_cache_t *cache = &regions[r].caches[i];
            cache->obj_size = slab_sizes[i];
            cache->objs_per_page = PAGE_SIZE / (sizeof(slab_obj_t) + slab_sizes[i] + SLAB_GUARD_SIZE);
            cache->free_list = NULL;
            cache->total_objs = 0;
            cache->free_objs = 0;
            spinlock_init(&cache->lock);
        }
    }
    
    slab_initialized = true;
    spinlock_init(&slab_vmm_lock);
    printk(SLAB_CLASS "Slab allocator initialized (VMM-based)\n");
    return 0;
}

void *kmalloc_region(size_t size, alloc_region_t region) {
    if (!slab_initialized || size == 0 || size > SLAB_MAX_SIZE || region >= ALLOC_REGION_COUNT)
        return NULL;
    
    size_t index = slab_size_to_index(size);
    slab_cache_t *cache = &regions[region].caches[index];
    
    spinlock_lock(&cache->lock);
    slab_obj_t *obj = slab_alloc_obj(cache, region);
    if (obj) {
        regions[region].total_allocated += cache->obj_size;
        if (regions[region].total_allocated > regions[region].peak_allocated)
            regions[region].peak_allocated = regions[region].total_allocated;
    }
    spinlock_unlock(&cache->lock);
    
    return obj ? (void *)((uint8_t *)obj + sizeof(slab_obj_t)) : NULL;
}

void *kmalloc(size_t size) {
    return kmalloc_region(size, ALLOC_REGION_KERNEL);
}

void *alloc_stack(size_t size) {
    return kmalloc_region(size, ALLOC_REGION_STACK);
}

void kfree(void *ptr) {
    if (!ptr || !slab_initialized) return;
    
    slab_obj_t *obj = (slab_obj_t *)((uint8_t *)ptr - sizeof(slab_obj_t));
    
    if (obj->magic != SLAB_MAGIC_ALLOC) {
        printk(KERN_ERR SLAB_CLASS "invalid magic 0x%x at %p\n", obj->magic, ptr);
        panic(SLAB_CLASS "invalid magic");
    }
    
    if (!slab_check_guards_internal(obj)) {
        printk(KERN_ERR SLAB_CLASS "guard corruption at %p\n", ptr);
        panic(SLAB_CLASS "guard corruption");
    }
    
    if (obj->region >= ALLOC_REGION_COUNT) {
        printk(KERN_ERR SLAB_CLASS "invalid region %d at %p\n", obj->region, ptr);
        panic(SLAB_CLASS "invalid region");
    }
    
    size_t index = slab_size_to_index(obj->size);
    slab_cache_t *cache = &regions[obj->region].caches[index];
    
    spinlock_lock(&cache->lock);
    
    obj->magic = SLAB_MAGIC_FREE;
    obj->next = cache->free_list;
    cache->free_list = obj;
    cache->free_objs++;
    
    regions[obj->region].total_allocated -= obj->size;
    
    spinlock_unlock(&cache->lock);
}

void free_stack(void *ptr) {
    kfree(ptr); // Same implementation, guards are checked
}

bool slab_check_guards(void *ptr) {
    if (!ptr) return false;
    
    slab_obj_t *obj = (slab_obj_t *)((uint8_t *)ptr - sizeof(slab_obj_t));
    return obj->magic == SLAB_MAGIC_ALLOC && slab_check_guards_internal(obj);
}

void slab_dump_stats(void) {
    printk(SLAB_CLASS "-- Slab Allocator Statistics: \n");
    
    for (int r = 0; r < ALLOC_REGION_COUNT; r++) {
        slab_region_t *region = &regions[r];
        printk(SLAB_CLASS "Region %s: allocated=%llu peak=%llu\n",
               region->name, region->total_allocated, region->peak_allocated);
        
        for (int i = 0; i < SLAB_COUNT; i++) {
            slab_cache_t *cache = &region->caches[i];
            if (cache->total_objs > 0) {
                printk(SLAB_CLASS "  Size %llu: total=%llu free=%llu\n",
                       cache->obj_size, cache->total_objs, cache->free_objs);
            }
        }
    }
}