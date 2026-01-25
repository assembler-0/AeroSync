#include <mm/zmm.h>
#include <mm/slub.h>
#include <mm/page.h>
#include <lib/string.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/printk.h>
#include <aerosync/classes.h>
#include <aerosync/spinlock.h>

#ifdef CONFIG_MM_ZMM

/*
 * ZMM Implementation
 * 
 * Uses a simplified RLE + Raw fallback for speed.
 * Storage is managed via kmalloc (for now, but using a custom slab cache).
 */

static struct kmem_cache *zmm_cache;
static spinlock_t zmm_lock = 0;

int zmm_init(void) {
    zmm_cache = kmem_cache_create("zmm_entries", 64, 0, SLAB_HWCACHE_ALIGN);
    printk(KERN_INFO VMM_CLASS "ZMM Anonymous memory compression pool initialized.\n");
    return 0;
}

struct zmm_entry {
    void *data;
    uint32_t size;
    bool is_rle;
};

/* Simple RLE for extremely fast compression of zero-heavy pages */
static uint32_t rle_compress(const uint8_t *src, uint8_t *dst, uint32_t src_len) {
    uint32_t i = 0, j = 0;
    while (i < src_len) {
        uint8_t count = 1;
        while (i + count < src_len && src[i + count] == src[i] && count < 255) {
            count++;
        }
        if (j + 2 > src_len) return 0; /* Compression failed/expanded */
        dst[j++] = count;
        dst[j++] = src[i];
        i += count;
    }
    return j;
}

static void rle_decompress(const uint8_t *src, uint32_t src_len, uint8_t *dst) {
    uint32_t i = 0, j = 0;
    while (i < src_len) {
        uint8_t count = src[i++];
        uint8_t val = src[i++];
        memset(dst + j, val, count);
        j += count;
    }
}

#ifndef ZMM_COMPRESSION_THRESHOLD
  #ifndef CONFIG_ZMM_COMPRESSION_THRESHOLD
    #define ZMM_COMPRESSION_THRESHOLD 75
  #else
    #define ZMM_COMPRESSION_THRESHOLD CONFIG_ZMM_COMPRESSION_THRESHOLD
  #endif
#endif

zmm_handle_t zmm_compress_folio(struct folio *folio) {
    uint8_t *src = folio_address(folio);
    uint8_t *tmp = kmalloc(PAGE_SIZE);
    if (!tmp) return 0;

    /* Try RLE */
    uint32_t csize = rle_compress(src, tmp, PAGE_SIZE);
    bool is_rle = true;

    /* If RLE failed or ratio is bad (> threshold), don't bother */
    if (csize == 0 || csize > (PAGE_SIZE * ZMM_COMPRESSION_THRESHOLD / 100)) {
        /* Raw fallback (not saving much memory, but keeps the infrastructure exercised) */
        /* For now, let's only store if it really compresses. */
        kfree(tmp);
        return 0;
    }

    struct zmm_entry *entry = kmem_cache_alloc(zmm_cache);
    if (!entry) {
        kfree(tmp);
        return 0;
    }

    entry->data = kmalloc(csize);
    if (!entry->data) {
        kmem_cache_free(zmm_cache, entry);
        kfree(tmp);
        return 0;
    }

    memcpy(entry->data, tmp, csize);
    entry->size = csize;
    entry->is_rle = is_rle;
    kfree(tmp);

    return (zmm_handle_t)entry;
}

int zmm_decompress_to_folio(zmm_handle_t handle, struct folio *folio) {
    struct zmm_entry *entry = (struct zmm_entry *)handle;
    if (!entry) return -1;

    uint8_t *dst = folio_address(folio);
    if (entry->is_rle) {
        rle_decompress(entry->data, entry->size, dst);
    } else {
        memcpy(dst, entry->data, entry->size);
    }

    return 0;
}

void zmm_free_handle(zmm_handle_t handle) {
    struct zmm_entry *entry = (struct zmm_entry *)handle;
    if (!entry) return;

    kfree(entry->data);
    kmem_cache_free(zmm_cache, entry);
}

#endif