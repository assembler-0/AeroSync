#pragma once

#include <aerosync/types.h>
#include <limine/limine.h>

#include <lib/printk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    uint32_t width, height;
    uint32_t pitch;
    uint32_t bpp;
} linearfb_font_t;

typedef struct {
    void *address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
} linearfb_surface_t;

int linearfb_init_standard(void *data);
void linearfb_cleanup(void);
int linearfb_is_initialized(void);

// probing
int linearfb_probe(void);

// Load font (bitmap, width, height, glyph count)
int linearfb_load_font(const linearfb_font_t* font, uint32_t count);

// Graphics Primitives
void linearfb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t linearfb_get_pixel(uint32_t x, uint32_t y);
void linearfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void linearfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void linearfb_draw_line_blend(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color);
void linearfb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void linearfb_draw_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color);
void linearfb_fill_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color);

typedef struct {
    uint8_t red_mask_size, red_mask_shift;
    uint8_t green_mask_size, green_mask_shift;
    uint8_t blue_mask_size, blue_mask_shift;
    uint8_t alpha_mask_size, alpha_mask_shift;
    uint16_t bpp;
} linearfb_color_format_t;

// Color utility
uint32_t linearfb_encode_color(const linearfb_color_format_t *fmt, uint8_t r, uint8_t g, uint8_t b);
uint32_t linearfb_encode_color_rgba(const linearfb_color_format_t *fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void linearfb_decode_color(const linearfb_color_format_t *fmt, uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b);
void linearfb_decode_color_rgba(const linearfb_color_format_t *fmt, uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a);

uint32_t linearfb_make_color(uint8_t r, uint8_t g, uint8_t b);
uint32_t linearfb_make_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
uint32_t linearfb_color_lerp(uint32_t c1, uint32_t c2, float t);
uint32_t linearfb_color_brightness(uint32_t color, float amount);

// Get resolution
void linearfb_get_resolution(uint32_t *width, uint32_t *height);
void linearfb_get_screen_surface(linearfb_surface_t *surface);
void linearfb_get_color_format(linearfb_color_format_t *fmt);

// Advanced Graphics Primitives
void linearfb_put_pixel_blend(uint32_t x, uint32_t y, uint32_t color);
void linearfb_draw_rect_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void linearfb_fill_rect_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void linearfb_fill_rect_gradient(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c1, uint32_t c2, int vertical);
void linearfb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
void linearfb_fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color);
void linearfb_draw_shadow_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t opacity);
void linearfb_blit(linearfb_surface_t *dst, linearfb_surface_t *src, uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);

// Draw text (console: col/row, graphics: x/y)
void linearfb_draw_text(const char *text, uint32_t x, uint32_t y, uint32_t color);

// --- Console mode API ---
void linearfb_console_set_cursor(uint32_t col, uint32_t row);
void linearfb_console_get_cursor(uint32_t *col, uint32_t *row);
void linearfb_console_clear(uint32_t color);
void linearfb_console_putc(char c);
void linearfb_console_puts(const char *s);

const printk_backend_t* linearfb_get_backend(void);

#ifdef __cplusplus
}
#endif
