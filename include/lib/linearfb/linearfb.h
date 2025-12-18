#pragma once

#include <kernel/types.h>
#include <limine/limine.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FB_MODE_CONSOLE,
    FB_MODE_GRAPHICS
} linearfb_mode_t;

typedef struct {
    uint8_t *data;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
} linearfb_font_t;

int linearfb_init_standard(void *data);

// Initialize framebuffer library with Limine framebuffer request
int linearfb_init(struct limine_framebuffer_request *fb_req);

// Set mode (console/graphics)
void linearfb_set_mode(linearfb_mode_t mode);

// probing
int linearfb_probe(void);

// Load font (bitmap, width, height, glyph count)
int linearfb_load_font(const linearfb_font_t* font, uint32_t count);

// Draw text (console: col/row, graphics: x/y)
void linearfb_draw_text(const char *text, uint32_t x, uint32_t y);

// Draw polygon (n points, filled or outline)
void linearfb_draw_polygon(const int *x, const int *y, size_t n, uint32_t color, int filled);

// --- Console mode API ---
void linearfb_console_set_cursor(uint32_t col, uint32_t row);
void linearfb_console_get_cursor(uint32_t *col, uint32_t *row);
void linearfb_console_clear(uint32_t color);
void linearfb_console_putc(char c);
void linearfb_console_puts(const char *s);

#ifdef __cplusplus
}
#endif
