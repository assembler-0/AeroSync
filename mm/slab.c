#include <arch/x64/smp.h>
#include <mm/slab.h>
#include <mm/vmalloc.h>
#include <arch/x64/mm/layout.h>
#include <arch/x64/mm/pmm.h>
#include <arch/x64/mm/paging.h>
#include <kernel/classes.h>
#include <lib/string.h>
#include <lib/printk.h>
#include <kernel/panic.h>
#include <kernel/fkx/fkx.h>

static kmem_cache_t *kmalloc_caches[9];  // 8, 16, 32... 2048
static kmem_cache_t *cache_cache;        // Cache for kmem_cache_t structures

// Helper: Get slab header from an object pointer (assumes 4KB alignment)
static inline slab_page_t *get_slab_from_obj(void *obj) {
    return (slab_page_t *)((uint64_t)obj & ~0xFFF);
}

static void *slab_alloc_page(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return NULL;
    return pmm_phys_to_virt(phys);
}

static slab_page_t *allocate_new_slab(kmem_cache_t *cache) {
    void *page_addr = slab_alloc_page();
    if (!page_addr) return NULL;

    slab_page_t *slab = (slab_page_t *)page_addr;
    slab->cache = cache;
    slab->in_use = 0;
    slab->next = NULL;
    slab->on_partial_list = 0;
    slab->cpu = -1;

    // Offset the first object to bypass the slab_page header
    // Ensure the first object is aligned
    size_t header_size = sizeof(slab_page_t);
    size_t offset = (header_size + (cache->align - 1)) & ~(cache->align - 1);
    
    uint8_t *first_obj = (uint8_t *)page_addr + offset;
    slab->free_list = (void **)first_obj;

    // Link objects in the page
    if (offset + cache->size > PAGE_SIZE) {
        slab->free_list = NULL;
        return slab;
    }

    uint32_t count = (PAGE_SIZE - offset) / cache->size;
    cache->objs_per_slab = count;
    
    uint8_t *curr = first_obj;
    for (uint32_t i = 0; i < count - 1; i++) {
        uint8_t *next = curr + cache->size;
        *(void **)curr = next; // Store pointer to next free object inside the object
        curr = next;
    }
    *(void **)curr = NULL; // Last object

    return slab;
}

void *kmem_cache_alloc(kmem_cache_t *cache) {
    uint64_t flags = save_irq_flags();
    uint64_t cpu = smp_is_active() ? smp_get_id() : 0;
    kmem_cpu_cache_t *cpu_cache = &cache->cpu_caches[cpu];

    // FAST PATH: Try CPU local cache
    if (cpu_cache->free_list) {
        void *obj = cpu_cache->free_list;
        cpu_cache->free_list = * (void **)obj;
        __atomic_add_fetch(&get_slab_from_obj(obj)->in_use, 1, __ATOMIC_RELAXED);
        restore_irq_flags(flags);
        return obj;
    }

    // MEDIUM PATH: Try to reclaim from the current CPU page (objects freed by other CPUs)
    if (cpu_cache->page) {
        spinlock_lock(&cache->node_lock);
        if (cpu_cache->page->free_list) {
            cpu_cache->free_list = cpu_cache->page->free_list;
            cpu_cache->page->free_list = NULL;
            spinlock_unlock(&cache->node_lock);
            
            void *obj = cpu_cache->free_list;
            cpu_cache->free_list = *(void **)obj;
            __atomic_add_fetch(&cpu_cache->page->in_use, 1, __ATOMIC_RELAXED);
            restore_irq_flags(flags);
            return obj;
        }
        // Slab is truly full, detach it
        slab_page_t *old_page = cpu_cache->page;
        old_page->cpu = -1;
        cpu_cache->page = NULL;
        
        // Check if someone freed an object JUST before we set cpu = -1
        if (old_page->free_list && !old_page->on_partial_list) {
            old_page->next = cache->partial_list;
            cache->partial_list = old_page;
            old_page->on_partial_list = 1;
        }
        spinlock_unlock(&cache->node_lock);
    }

    // SLOW PATH: Try Global Partial List
    spinlock_lock(&cache->node_lock);
    if (cache->partial_list) {
        slab_page_t *slab = cache->partial_list;
        cache->partial_list = slab->next;
        slab->next = NULL;
        slab->on_partial_list = 0;
        
        void *obj = slab->free_list;
        slab->free_list = *(void **)obj;
        __atomic_add_fetch(&slab->in_use, 1, __ATOMIC_RELAXED);
        
        // If slab still has space, set as current CPU slab
        if (slab->free_list) {
            cpu_cache->page = slab;
            slab->cpu = (int)cpu;
            cpu_cache->free_list = slab->free_list;
            slab->free_list = NULL;
        }

        spinlock_unlock(&cache->node_lock);
        restore_irq_flags(flags);
        return obj;
    }
    spinlock_unlock(&cache->node_lock);

    // VERY SLOW PATH: Allocate new page
    slab_page_t *new_slab = allocate_new_slab(cache);
    if (!new_slab || !new_slab->free_list) {
        restore_irq_flags(flags);
        return NULL;
    }

    void *obj = new_slab->free_list;
    new_slab->free_list = *(void **)obj;
    __atomic_add_fetch(&new_slab->in_use, 1, __ATOMIC_RELAXED);

    if (new_slab->free_list) {
        cpu_cache->page = new_slab;
        new_slab->cpu = (int)cpu;
        cpu_cache->free_list = new_slab->free_list;
        new_slab->free_list = NULL;
    }

    restore_irq_flags(flags);
    return obj;
}

void kmem_cache_free(kmem_cache_t *cache, void *obj) {
    if (!obj) return;

    uint64_t flags = save_irq_flags();
    slab_page_t *slab = get_slab_from_obj(obj);
    uint64_t cpu = smp_is_active() ? smp_get_id() : 0;
    kmem_cpu_cache_t *cpu_cache = &cache->cpu_caches[cpu];

    // If the object belongs to the page currently being used by THIS CPU
    if (slab == cpu_cache->page) {
        *(void **)obj = cpu_cache->free_list;
        cpu_cache->free_list = obj;
    } else {
        // Return to the slab's local free list
        spinlock_lock(&cache->node_lock);
        *(void **)obj = slab->free_list;
        slab->free_list = obj;
        
        // If slab was full and is now partial, move it to partial list
        // ONLY if it's not a private page for some CPU
        if (slab->free_list != NULL && !slab->on_partial_list && slab->cpu == -1) {
            slab->next = cache->partial_list;
            cache->partial_list = slab;
            slab->on_partial_list = 1;
        }
        spinlock_unlock(&cache->node_lock);
    }

    __atomic_sub_fetch(&slab->in_use, 1, __ATOMIC_RELAXED);
    restore_irq_flags(flags);
}

kmem_cache_t *kmem_cache_create(const char *name, size_t size, size_t align) {
    kmem_cache_t *cache = kmem_cache_alloc(cache_cache);
    cache->name = name;
    cache->size = (size < sizeof(void *)) ? sizeof(void *) : size;
    cache->size = (cache->size + (align - 1)) & ~(align - 1);
    cache->align = align;
    cache->partial_list = NULL;
    spinlock_init(&cache->node_lock);
    memset(cache->cpu_caches, 0, sizeof(cache->cpu_caches));
    return cache;
}

// Global Kmalloc Initialization
void slab_init(void) {
    // 1. Manually set up the cache for kmem_cache_t (bootstrap)
    static kmem_cache_t bootstrap_cache;
    cache_cache = &bootstrap_cache;
    cache_cache->name = "kmem_cache";
    cache_cache->size = sizeof(kmem_cache_t);
    cache_cache->align = 8;
    spinlock_init(&cache_cache->node_lock);
    memset(cache_cache->cpu_caches, 0, sizeof(cache_cache->cpu_caches));

    // 2. Initialize size buckets
    char *names[] = {"kmalloc-8", "kmalloc-16", "kmalloc-32", "kmalloc-64", 
                     "kmalloc-128", "kmalloc-256", "kmalloc-512", "kmalloc-1k", 
                     "kmalloc-2k"};
    
    for (int i = 0; i < 9; i++) {
        kmalloc_caches[i] = kmem_cache_create(names[i], 8 << i, 8);
    }
    
    printk(SLAB_CLASS "SLUB allocator initialized (SMP %s)\n", smp_is_active() ? "Active" : "Inactive");
}

void *kmalloc(size_t size) {
    if (size > SLAB_MAX_SIZE) return vmalloc(size);
    for (int i = 0; i < 9; i++) {
        if (size <= kmalloc_caches[i]->size) 
            return kmem_cache_alloc(kmalloc_caches[i]);
    }
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    uint64_t addr = (uint64_t)ptr;
    if (addr >= VMALLOC_VIRT_BASE && addr < VMALLOC_VIRT_END) {
        vfree(ptr);
        return;
    }

    slab_page_t *slab = get_slab_from_obj(ptr);
    kmem_cache_free(slab->cache, ptr);
}
EXPORT_SYMBOL(kmalloc);
EXPORT_SYMBOL(kfree);
