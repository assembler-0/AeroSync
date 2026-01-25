#pragma once

#include <mm/page.h>

/**
 * ZMM - Anonymous Memory Compression
 * 
 * Provides a mechanism to reclaim cold anonymous pages by compressing 
 * them into a dedicated in-memory pool.
 */

#ifdef CONFIG_MM_ZMM

typedef uint64_t zmm_handle_t;

int zmm_init(void);

/**
 * zmm_compress_folio - Attempts to compress and store a folio.
 * Returns a handle > 0 on success, or 0 on failure.
 */
zmm_handle_t zmm_compress_folio(struct folio *folio);

/**
 * zmm_decompress_to_folio - Decompresses a handle into a pre-allocated folio.
 * Returns 0 on success, -1 on failure.
 */
int zmm_decompress_to_folio(zmm_handle_t handle, struct folio *folio);

/**
 * zmm_free_handle - Frees a compressed block.
 */
void zmm_free_handle(zmm_handle_t handle);

#else

typedef uint64_t zmm_handle_t;
static inline int zmm_init(void) { return 0; }
static inline zmm_handle_t zmm_compress_folio(struct folio *folio) { (void)folio; return 0; }
static inline int zmm_decompress_to_folio(zmm_handle_t handle, struct folio *folio) { (void)handle; (void)folio; return -1; }
static inline void zmm_free_handle(zmm_handle_t handle) { (void)handle; }

#endif
