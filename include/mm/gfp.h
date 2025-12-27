#pragma once

#include <kernel/types.h>

typedef unsigned int gfp_t;

/* Plain integer GFP bitmasks. Do not use this directly. */
#define ___GFP_DMA      0x01u
#define ___GFP_HIGHMEM  0x02u
#define ___GFP_DMA32    0x04u
#define ___GFP_MOVABLE  0x08u
#define ___GFP_RECLAIM  0x10u
#define ___GFP_HIGH     0x20u
#define ___GFP_IO       0x40u
#define ___GFP_FS       0x80u
#define ___GFP_ZERO     0x100u
#define ___GFP_ATOMIC   0x200u // Atomicity requirement (no sleep)

/*
 * Standard groups
 */
#define GFP_ATOMIC      (___GFP_ATOMIC | ___GFP_HIGH)
#define GFP_KERNEL      (___GFP_IO | ___GFP_FS)
#define GFP_USER        (___GFP_IO | ___GFP_FS | ___GFP_MOVABLE)
#define GFP_HIGHUSER    (GFP_USER | ___GFP_HIGHMEM)
#define GFP_DMA         ___GFP_DMA
#define GFP_DMA32       ___GFP_DMA32

/*
 * Zone modifiers
 */
#define GFP_ZONEMASK    (___GFP_DMA | ___GFP_DMA32 | ___GFP_HIGHMEM | ___GFP_MOVABLE)
