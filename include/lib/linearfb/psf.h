#pragma once

#include <kernel/types.h>

#define PSF1_MAGIC0 0x36
#define PSF1_MAGIC1 0x04

#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_MODEHASSEQ 0x04
#define PSF1_MAXMODE    0x05

#define PSF1_SEPARATOR  0xFFFF
#define PSF1_STARTSEQ   0xFFFE

typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} __attribute__((packed)) psf1_header_t;

#define PSF2_MAGIC0 0x72
#define PSF2_MAGIC1 0xb5
#define PSF2_MAGIC2 0x4a
#define PSF2_MAGIC3 0x86

/* bits used in flags */
#define PSF2_HAS_UNICODE_TABLE 0x01

/* max version recognized so far */
#define PSF2_MAXVERSION 0

/* UTF8 separators */
#define PSF2_SEPARATOR  0xFF
#define PSF2_STARTSEQ   0xFE

typedef struct {
    uint8_t magic[4];
    uint32_t version;
    uint32_t headersize;    /* offset of bitmaps in file */
    uint32_t flags;
    uint32_t length;        /* number of glyphs */
    uint32_t charsize;      /* number of bytes for each character */
    uint32_t height, width; /* max dimensions of glyphs */
    /* charsize = height * ((width + 7) / 8) */
} __attribute__((packed)) psf2_header_t;

/* Internal font structure used by linearfb */
typedef struct {
    uint8_t *buffer;      /* Pointer to the font file data */
    uint8_t *glyph_data;  /* Pointer to the start of glyph data */
    uint32_t num_glyphs;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_glyph;
    uint32_t bytes_per_line; /* Stride */
    uint32_t flags;
} psf_font_t;

int psf_parse(const void *data, size_t size, psf_font_t *font);
