/// SPDX-License-Identifier: GPL-2.0-only
/**
 * linearfb - PSF Font Support
 *
 * @file drivers/graphics/drm/linearfb/psf.c
 * @brief PC Screen Font (PSF) 1 & 2 parser
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/errno.h>
#include <lib/linearfb/psf.h>
#include <lib/string.h>

int psf_parse(const void *data, size_t size, psf_font_t *font) {
    if (!data || !font || size < sizeof(psf1_header_t)) return -EINVAL;
    
    const uint8_t *bytes = (const uint8_t *)data;

    // Check for PSF1
    if (bytes[0] == PSF1_MAGIC0 && bytes[1] == PSF1_MAGIC1) {
        const psf1_header_t *h = (const psf1_header_t *)data;
        font->buffer = (uint8_t *)data;
        font->glyph_data = (uint8_t *)data + sizeof(psf1_header_t);
        font->flags = 0;
        
        font->num_glyphs = (h->mode & PSF1_MODE512) ? 512 : 256;
        font->bytes_per_glyph = h->charsize;
        font->height = h->charsize;
        font->width = 8; // PSF1 is always 8 pixels wide
        font->bytes_per_line = 1;
        
        // Validation
        size_t expected = sizeof(psf1_header_t) + font->num_glyphs * font->bytes_per_glyph;
        if (size < expected) return -EINVAL;
        
        return 0;
    }

    // Check for PSF2
    if (size >= sizeof(psf2_header_t) && 
        bytes[0] == PSF2_MAGIC0 && bytes[1] == PSF2_MAGIC1 && 
        bytes[2] == PSF2_MAGIC2 && bytes[3] == PSF2_MAGIC3) {
        
        const psf2_header_t *h = (const psf2_header_t *)data;
        font->buffer = (uint8_t *)data;
        font->glyph_data = (uint8_t *)data + h->headersize;
        font->flags = h->flags;
        font->num_glyphs = h->length;
        font->bytes_per_glyph = h->charsize;
        font->height = h->height;
        font->width = h->width;
        font->bytes_per_line = (h->width + 7) / 8;

        // Validation
        size_t expected = h->headersize + font->num_glyphs * font->bytes_per_glyph;
        if (size < expected) return -EINVAL;

        return 0;
    }

    return -EINVAL; // Unknown format
}
