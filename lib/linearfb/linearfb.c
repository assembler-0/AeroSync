/// SPDX-License-Identifier: MIT
/**
 * linearfb - Linear Framebuffer library
 *
 * @file lib/linearfb/linearfb.c
 * @brief simple linear framebuffer graphics and console library
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the linearfb project.
 */

#include <lib/linearfb/linearfb.h>
#include <lib/string.h>
#include <kernel/spinlock.h>
#include <linearfb/font.h>
#include <lib/printk.h>

static int fb_initialized = 0;
static struct limine_framebuffer *fb = NULL;
static linearfb_mode_t fb_mode = FB_MODE_CONSOLE;
static linearfb_font_t fb_font = {0};
static uint32_t font_glyph_count = 0;
static uint32_t font_glyph_w = 0, font_glyph_h = 0;

// --- Console state ---
static uint32_t console_col = 0, console_row = 0;
static uint32_t console_cols = 0, console_rows = 0;
static spinlock_t fb_lock = 0;
static uint32_t console_bg = 0x00000000;
static uint32_t console_fg = 0xFFFFFFFF;

#define CONSOLE_BUF_MAX (128 * 1024)
static char console_buffer[CONSOLE_BUF_MAX];

extern volatile struct limine_framebuffer_request framebuffer_request;

// Forward declaration
static void linearfb_console_redraw(void);

int linearfb_init_standard(void *data) {
  (void)data;

  linearfb_init((struct limine_framebuffer_request *)&framebuffer_request);

  linearfb_font_t font = {
    .width = 8, .height = 16, .data = (uint8_t *)console_font};
  linearfb_load_font(&font, 256);
  linearfb_set_mode(FB_MODE_CONSOLE);
  linearfb_console_clear(0x00000000);
  linearfb_console_set_cursor(0, 0);

  return fb ? 0 : -1;
}

int linearfb_is_initialized(void) {
    return fb_initialized;
}

static printk_backend_t fb_backend = {
    .name = "linearfb",
    .priority = 100,
    .putc = linearfb_console_putc,
    .probe = linearfb_probe,
    .init = linearfb_init_standard,
    .cleanup = linearfb_cleanup,
    .is_active = linearfb_is_initialized
};

void linearfb_cleanup(void) {
    fb_initialized = 0;
    fb = NULL;
}

const printk_backend_t* linearfb_get_backend(void) {
    return &fb_backend;
}

int linearfb_probe(void) {
    return framebuffer_request.response ? 1 : 0;
}

void linearfb_console_set_cursor(uint32_t col, uint32_t row) {
    if (col < console_cols) __atomic_store_n(&console_col, col, __ATOMIC_SEQ_CST);
    if (row < console_rows) __atomic_store_n(&console_row, row, __ATOMIC_SEQ_CST);
}

void linearfb_console_get_cursor(uint32_t *col, uint32_t *row) {
    if (col) *col = console_col;
    if (row) *row = console_row;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || x >= fb->width || y >= fb->height) return;
    uint8_t *p = (uint8_t*)fb->address + y * fb->pitch + x * (fb->bpp / 8);
    memcpy(p, &color, fb->bpp / 8);
}

void linearfb_console_clear(uint32_t color) {
    if (!fb) return;
    for (uint32_t y = 0; y < fb->height; ++y) {
        for (uint32_t x = 0; x < fb->width; ++x) {
            putpixel(x, y, color);
        }
    }
    // Clear backbuffer
    memset(console_buffer, ' ', sizeof(console_buffer));
    console_col = 0;
    console_row = 0;
    console_bg = color;
}

static void fast_rect_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (!fb) return;

  if (fb->bpp != 32) {
    // Fall back to putpixel for non-32bpp modes
    for (uint32_t dy = 0; dy < h; ++dy) {
      for (uint32_t dx = 0; dx < w; ++dx) {
        putpixel(x + dx, y + dy, color);
      }
    }
    return;
  }

  uint32_t *row_ptr = (uint32_t*)((uint8_t*)fb->address + y * fb->pitch + x * 4);

  for (uint32_t i = 0; i < h; ++i) {
    // Optimized: standard memset is usually optimized for 32/64-bit fills
    // or a tight loop here is much faster than putpixel
    for (uint32_t j = 0; j < w; ++j) {
      row_ptr[j] = color;
    }
    row_ptr = (uint32_t*)((uint8_t*)row_ptr + fb->pitch);
  }
}

static void linearfb_draw_glyph_at(uint32_t col, uint32_t row, char c) {
    if (!fb || !fb_font.data) return;

    // Boundary check
    if (col >= console_cols || row >= console_rows) return;

    uint32_t px = col * font_glyph_w;
    uint32_t py = row * font_glyph_h;

    uint8_t ch = (uint8_t)c;
    if (ch >= font_glyph_count) ch = '?';
    const uint8_t *glyph = fb_font.data + ch * font_glyph_h;

    uint8_t *draw_ptr = (uint8_t*)fb->address + py * fb->pitch + px * 4;

    for (uint32_t r = 0; r < font_glyph_h; ++r) {
        uint32_t *pixel_ptr = (uint32_t*)draw_ptr;
        uint8_t bits = glyph[r];
        for (uint32_t cx = 0; cx < font_glyph_w; ++cx) {
            uint32_t color = (bits & (1 << (7 - cx))) ? console_fg : console_bg;
            *pixel_ptr++ = color;
        }
        draw_ptr += fb->pitch;
    }
}

static void linearfb_console_redraw(void) {
    if (!fb) return;
    for (uint32_t y = 0; y < console_rows; ++y) {
        for (uint32_t x = 0; x < console_cols; ++x) {
            size_t idx = y * console_cols + x;
            if (idx < CONSOLE_BUF_MAX) {
                linearfb_draw_glyph_at(x, y, console_buffer[idx]);
            }
        }
    }
}

static void linearfb_console_scroll(void) {
    if (console_rows <= 1) return;

    // Shift buffer up
    size_t line_size = console_cols;
    size_t total_chars = console_rows * console_cols;
    
    // Ensure we don't overflow the buffer size
    if (total_chars > CONSOLE_BUF_MAX) total_chars = CONSOLE_BUF_MAX;

    size_t copy_size = (console_rows - 1) * line_size;

    if (copy_size < CONSOLE_BUF_MAX) {
        memmove(console_buffer, console_buffer + line_size, copy_size);
        memset(console_buffer + copy_size, ' ', line_size);
    }
    
    // Redraw screen from buffer
    linearfb_console_redraw();

    console_row = console_rows - 1;
    console_col = 0;
}

void linearfb_console_putc(char c) {
    // Acquire Lock
    irq_flags_t flags = spinlock_lock_irqsave(&fb_lock);

    if (fb_mode != FB_MODE_CONSOLE || !fb_font.data) {
        spinlock_unlock_irqrestore(&fb_lock, flags);
        return;
    }

    if (c == '\n') {
        console_col = 0;
        if (++console_row >= console_rows) linearfb_console_scroll();
        spinlock_unlock_irqrestore(&fb_lock, flags);
        return;
    }
    if (c == '\r') {
        console_col = 0;
        spinlock_unlock_irqrestore(&fb_lock, flags);
        return;
    }

    // Update buffer
    size_t buf_idx = console_row * console_cols + console_col;
    if (buf_idx < CONSOLE_BUF_MAX) {
        console_buffer[buf_idx] = c;
    }

    // Draw directly
    linearfb_draw_glyph_at(console_col, console_row, c);

    if (++console_col >= console_cols) {
        console_col = 0;
        if (++console_row >= console_rows) linearfb_console_scroll();
    }

    spinlock_unlock_irqrestore(&fb_lock, flags);
}

void linearfb_console_puts(const char *s) {
    while (*s) linearfb_console_putc(*s++);
}

#define abs(x) ((x) >= 0 ? (x) : -(x))

int linearfb_init(struct limine_framebuffer_request *fb_req) {
    if (!fb_req || !fb_req->response || fb_req->response->framebuffer_count == 0)
        return -1;
    fb = fb_req->response->framebuffers[0];
    // Update console cols/rows if font is ready
    if (fb && font_glyph_w && font_glyph_h) {
        console_cols = fb->width / font_glyph_w;
        console_rows = fb->height / font_glyph_h;
    }
    fb_initialized = 1;
    spinlock_init(&fb_lock);
    return 0;
}

void linearfb_set_mode(const linearfb_mode_t mode) {
    __atomic_store_n(&fb_mode, mode, __ATOMIC_SEQ_CST);
}

int linearfb_load_font(const linearfb_font_t* font, const uint32_t count) {
    if (!font)
        return -1;
    fb_font = *font;
    font_glyph_w = font->width;
    font_glyph_h = font->height;
    font_glyph_count = count;
    // Update console cols/rows if fb is ready
    if (fb && font_glyph_w && font_glyph_h) {
        console_cols = fb->width / font_glyph_w;
        console_rows = fb->height / font_glyph_h;
    }
    return 0;
}

void linearfb_draw_text(const char *text, uint32_t x, uint32_t y) {
    if (fb_mode == FB_MODE_CONSOLE) return; // No drawing in console mode
    if (!fb_font.data) return;
    for (size_t i = 0; text[i]; ++i) {
        uint8_t ch = (uint8_t)text[i];
        if (ch >= font_glyph_count) ch = '?';
        const uint8_t *glyph = fb_font.data + ch * font_glyph_h;
        for (uint32_t row = 0; row < font_glyph_h; ++row) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < font_glyph_w; ++col) {
                if (bits & (1 << (7 - col)))
                    putpixel(x + col, y + row, 0xFFFFFFFF);
            }
        }
        x += font_glyph_w;
    }
}

// Bresenham line
static void fb_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    while (1) {
        putpixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Simple polygon fill (scanline, convex only)
static void fb_fill_polygon(const int *x, const int *y, size_t n, uint32_t color) {
    int min_y = y[0], max_y = y[0];
    for (size_t i = 1; i < n; ++i) {
        if (y[i] < min_y) min_y = y[i];
        if (y[i] > max_y) max_y = y[i];
    }
    for (int yy = min_y; yy <= max_y; ++yy) {
        int nodes[64], nodes_n = 0;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            if ((y[i] < yy && y[j] >= yy) || (y[j] < yy && y[i] >= yy)) {
                int xval = x[i] + (yy - y[i]) * (x[j] - x[i]) / (y[j] - y[i]);
                if (nodes_n < 64) nodes[nodes_n++] = xval;
            }
        }
        for (int k = 0; k + 1 < nodes_n; k += 2) {
            if (nodes[k] > nodes[k + 1]) {
                int tmp = nodes[k]; nodes[k] = nodes[k + 1]; nodes[k + 1] = tmp;
            }
            for (int xx = nodes[k]; xx <= nodes[k + 1]; ++xx)
                putpixel(xx, yy, color);
        }
    }
}

void linearfb_draw_polygon(const int *x, const int *y, size_t n, uint32_t color, int filled) {
    if (fb_mode == FB_MODE_CONSOLE) return;
    if (n < 2) return;
    if (filled) fb_fill_polygon(x, y, n, color);
    for (size_t i = 0; i < n; ++i) {
        size_t j = (i + 1) % n;
        fb_draw_line(x[i], y[i], x[j], y[j], color);
    }
}