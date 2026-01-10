///SPDX-License-Identifier: GPL-2.0-only
/**
 * linearfb - Linear Framebuffer library
 *
 * @file lib/linearfb/linearfb.c
 * @brief simple linear framebuffer graphics and console library
 * @copyright (C) 2025 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <lib/linearfb/linearfb.h>
#include <aerosync/fkx/fkx.h>
#include <lib/linearfb/psf.h>
#include <lib/string.h>
#include <lib/math.h>
#include <mm/vmalloc.h>
#include <arch/x86_64/mm/pmm.h>
#include <aerosync/spinlock.h>

extern const uint8_t embedded_console_font[];
extern const uint32_t embedded_console_font_size;

static int fb_initialized = 0;
static struct limine_framebuffer *fb = NULL;
static linearfb_font_t fb_font = {0};
static uint32_t font_glyph_count = 0;
static uint32_t font_glyph_w = 0, font_glyph_h = 0;
static uint32_t font_pitch = 0;

// --- Console state ---
static uint32_t console_col = 0, console_row = 0;
static uint32_t console_cols = 0, console_rows = 0;
static volatile int fb_lock = 0;
static uint32_t console_bg = 0x00000000;
static uint32_t console_fg = 0xFFFFFFFF;

#define CONSOLE_BUF_MAX (128 * 1024)
static char console_buffer[CONSOLE_BUF_MAX];
static void *shadow_fb = NULL;

// This is provided by the kernel via the API, but we can also use a symbol lookup if we wanted.
// For now, let's keep the framebuffer_request as a pointer that we set in mod_init.
static volatile struct limine_framebuffer_request *framebuffer_request = NULL;

static int linearfb_init(volatile struct limine_framebuffer_request *fb_req) {
  if (!fb_req || !fb_req->response || fb_req->response->framebuffer_count == 0)
    return -1;
  fb = fb_req->response->framebuffers[0];

  // Remap framebuffer to Write-Combining (WC) for performance
  size_t size = fb->height * fb->pitch;
  uint64_t phys = pmm_virt_to_phys(fb->address);
  if (phys) {
      void *wc_addr = viomap_wc(phys, size);
      if (wc_addr) {
          fb->address = wc_addr;
      }
  }

  // Allocate shadow framebuffer for fast scrolling and redrawing
  if (shadow_fb) {
      vfree(shadow_fb);
  }
  shadow_fb = vmalloc(size);
  if (shadow_fb) {
      memset(shadow_fb, 0, size);
  }

  if (fb && font_glyph_w && font_glyph_h) {
    console_cols = fb->width / font_glyph_w;
    console_rows = fb->height / font_glyph_h;
  }
  fb_initialized = 1;
  spinlock_init(&fb_lock);
  return 0;
}

int linearfb_init_standard(void *data) {
  (void) data;
  linearfb_init(framebuffer_request);
  
  psf_font_t psf;
  if (psf_parse(embedded_console_font, embedded_console_font_size, &psf) == 0) {
      linearfb_font_t font = {
          .width = psf.width,
          .height = psf.height,
          .data = psf.glyph_data,
          .pitch = psf.bytes_per_line,
          .bpp = 1
      };
      linearfb_load_font(&font, psf.num_glyphs);
  }

  linearfb_console_clear(0x00000000);
  linearfb_console_set_cursor(0, 0);
  return fb ? 0 : -1;
}

int linearfb_is_initialized(void) {
  return fb_initialized;
}

void linearfb_cleanup(void) {
  fb_initialized = 0;
  if (shadow_fb) {
      vfree(shadow_fb);
      shadow_fb = NULL;
  }
  fb = NULL;
}

int linearfb_probe(void) {
  return (framebuffer_request && framebuffer_request->response) ? 1 : 0;
}

void linearfb_console_set_cursor(uint32_t col, uint32_t row) {
  if (col < console_cols) __atomic_store_n(&console_col, col, __ATOMIC_SEQ_CST);
  if (row < console_rows) __atomic_store_n(&console_row, row, __ATOMIC_SEQ_CST);
}

void linearfb_console_get_cursor(uint32_t *col, uint32_t *row) {
  if (col) *col = console_col;
  if (row) *row = console_row;
}

uint32_t linearfb_make_color(uint8_t r, uint8_t g, uint8_t b) {
    return (0xFFU << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
EXPORT_SYMBOL(linearfb_make_color);

uint32_t linearfb_make_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
EXPORT_SYMBOL(linearfb_make_color_rgba);

uint32_t linearfb_color_lerp(uint32_t c1, uint32_t c2, float t) {
    uint8_t r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF, a1 = (c1 >> 24) & 0xFF;
    uint8_t r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF, a2 = (c2 >> 24) & 0xFF;

    return linearfb_make_color_rgba(
        r1 + (r2 - r1) * t,
        g1 + (g2 - g1) * t,
        b1 + (b2 - b1) * t,
        a1 + (a2 - a1) * t
    );
}
EXPORT_SYMBOL(linearfb_color_lerp);

uint32_t linearfb_color_brightness(uint32_t color, float amount) {
    uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF, a = (color >> 24) & 0xFF;
    r = clamp((int)(r * amount), 0, 255);
    g = clamp((int)(g * amount), 0, 255);
    b = clamp((int)(b * amount), 0, 255);
    return linearfb_make_color_rgba(r, g, b, a);
}
EXPORT_SYMBOL(linearfb_color_brightness);

void linearfb_get_resolution(uint32_t *width, uint32_t *height) {
    if (width) *width = fb ? fb->width : 0;
    if (height) *height = fb ? fb->height : 0;
}
EXPORT_SYMBOL(linearfb_get_resolution);

void linearfb_get_screen_surface(linearfb_surface_t *surface) {
    if (!fb || !surface) return;
    surface->address = fb->address;
    surface->width = fb->width;
    surface->height = fb->height;
    surface->pitch = fb->pitch;
    surface->bpp = fb->bpp;
}
EXPORT_SYMBOL(linearfb_get_screen_surface);

void linearfb_blit(linearfb_surface_t *dst, linearfb_surface_t *src, uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
    if (!dst || !src) return;    
    // Bounds check
    if (dx >= dst->width || dy >= dst->height) return;
    if (sx >= src->width || sy >= src->height) return;
    
    if (dx + w > dst->width) w = dst->width - dx;
    if (dy + h > dst->height) h = dst->height - dy;
    if (sx + w > src->width) w = src->width - sx;
    if (sy + h > src->height) h = src->height - sy;

    uint32_t bpp_bytes = dst->bpp / 8;
    for (uint32_t i = 0; i < h; i++) {
        uint8_t *dst_ptr = (uint8_t *)dst->address + (dy + i) * dst->pitch + dx * bpp_bytes;
        uint8_t *src_ptr = (uint8_t *)src->address + (sy + i) * src->pitch + sx * bpp_bytes;

        if (shadow_fb && dst->address == fb->address) {
            uint8_t *sp = (uint8_t *)shadow_fb + (dy + i) * dst->pitch + dx * bpp_bytes;
            memcpy(sp, src_ptr, w * bpp_bytes);
        }

        memcpy(dst_ptr, src_ptr, w * bpp_bytes);
    }
}
EXPORT_SYMBOL(linearfb_blit);

void linearfb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  if (!fb || x >= fb->width || y >= fb->height) return;

  if (shadow_fb) {
      uint8_t *sp = (uint8_t *)shadow_fb + y * fb->pitch + x * (fb->bpp / 8);
      if (fb->bpp == 32) {
          *(uint32_t *)sp = color;
      } else {
          memcpy(sp, &color, fb->bpp / 8);
      }
  }

  uint8_t *p = (uint8_t *) fb->address + y * fb->pitch + x * (fb->bpp / 8);
  if (fb->bpp == 32) {
      *(uint32_t*)p = color;
  } else {
      memcpy(p, &color, fb->bpp / 8);
  }
}
EXPORT_SYMBOL(linearfb_put_pixel);

void linearfb_put_pixel_blend(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb || x >= fb->width || y >= fb->height) return;
    uint8_t alpha = (color >> 24) & 0xFF;
    if (alpha == 255) {
        linearfb_put_pixel(x, y, color);
        return;
    }
    if (alpha == 0) return;

    uint32_t bg = linearfb_get_pixel(x, y);
    uint32_t r = (((color >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha)) >> 8;
    uint32_t g = (((color >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha)) >> 8;
    uint32_t b = ((color & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha)) >> 8;

    linearfb_put_pixel(x, y, (0xFFU << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}
EXPORT_SYMBOL(linearfb_put_pixel_blend);

void linearfb_draw_rect_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    for (uint32_t i = 0; i < w; i++) {
        linearfb_put_pixel_blend(x + i, y, color);
        linearfb_put_pixel_blend(x + i, y + h - 1, color);
    }
    for (uint32_t i = 1; i < h - 1; i++) {
        linearfb_put_pixel_blend(x, y + i, color);
        linearfb_put_pixel_blend(x + w - 1, y + i, color);
    }
}
EXPORT_SYMBOL(linearfb_draw_rect_blend);

void linearfb_fill_rect_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb) return;
    for (uint32_t i = 0; i < h; i++) {
        for (uint32_t j = 0; j < w; j++) {
            linearfb_put_pixel_blend(x + j, y + i, color);
        }
    }
}
EXPORT_SYMBOL(linearfb_fill_rect_blend);

uint32_t linearfb_get_pixel(uint32_t x, uint32_t y) {
    if (!fb || x >= fb->width || y >= fb->height) return 0;

    if (shadow_fb) {
        uint8_t *sp = (uint8_t *) shadow_fb + y * fb->pitch + x * (fb->bpp / 8);
        uint32_t color = 0;
        memcpy(&color, sp, fb->bpp / 8);
        return color;
    }

    uint8_t *p = (uint8_t *) fb->address + y * fb->pitch + x * (fb->bpp / 8);
    uint32_t color = 0;
    memcpy(&color, p, fb->bpp / 8);
    return color;
}
EXPORT_SYMBOL(linearfb_get_pixel);

void linearfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        linearfb_put_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
EXPORT_SYMBOL(linearfb_draw_line);

void linearfb_draw_line_blend(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
    int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        linearfb_put_pixel_blend(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
EXPORT_SYMBOL(linearfb_draw_line_blend);

void linearfb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (w == 0 || h == 0) return;
    linearfb_draw_line(x, y, x + w - 1, y, color);
    linearfb_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
    linearfb_draw_line(x, y, x, y + h - 1, color);
    linearfb_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}
EXPORT_SYMBOL(linearfb_draw_rect);

void linearfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb) return;    
    // Simple clipping
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;
    if (w == 0 || h == 0) return;

    if (fb->bpp == 32) {
        for (uint32_t i = 0; i < h; i++) {
            if (shadow_fb) {
                uint32_t *sp = (uint32_t *)((uint8_t *)shadow_fb + (y + i) * fb->pitch + x * 4);
                memset32(sp, color, w);
            }
            uint32_t *p = (uint32_t *)((uint8_t *)fb->address + (y + i) * fb->pitch + x * 4);
            memset32(p, color, w);
        }
    } else {
        for (uint32_t i = 0; i < h; ++i) {
            for (uint32_t j = 0; j < w; ++j) {
                linearfb_put_pixel(x + j, y + i, color);
            }
        }
    }
}
EXPORT_SYMBOL(linearfb_fill_rect);

void linearfb_fill_rect_gradient(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c1, uint32_t c2, int vertical) {
    if (!fb) return;
    if (vertical) {
        for (uint32_t i = 0; i < h; ++i) {
            uint32_t color = linearfb_color_lerp(c1, c2, (float)i / h);
            for (uint32_t j = 0; j < w; ++j) {
                linearfb_put_pixel(x + j, y + i, color);
            }
        }
    } else {
        for (uint32_t j = 0; j < w; ++j) {
            uint32_t color = linearfb_color_lerp(c1, c2, (float)j / w);
            for (uint32_t i = 0; i < h; ++i) {
                linearfb_put_pixel(x + j, y + i, color);
            }
        }
    }
}
EXPORT_SYMBOL(linearfb_fill_rect_gradient);

void linearfb_draw_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        linearfb_put_pixel(xc + x, yc + y, color);
        linearfb_put_pixel(xc - x, yc + y, color);
        linearfb_put_pixel(xc + x, yc - y, color);
        linearfb_put_pixel(xc - x, yc - y, color);
        linearfb_put_pixel(xc + y, yc + x, color);
        linearfb_put_pixel(xc - y, yc + x, color);
        linearfb_put_pixel(xc + y, yc - x, color);
        linearfb_put_pixel(xc - y, yc - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}
EXPORT_SYMBOL(linearfb_draw_circle);

void linearfb_fill_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        linearfb_draw_line(xc - x, yc + y, xc + x, yc + y, color);
        linearfb_draw_line(xc - x, yc - y, xc + x, yc - y, color);
        linearfb_draw_line(xc - y, yc + x, xc + y, yc + x, color);
        linearfb_draw_line(xc - y, yc - x, xc + y, yc - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}
EXPORT_SYMBOL(linearfb_fill_circle);

static void linearfb_draw_corner(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color, int corner) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        switch (corner) {
            case 0: // Top-left
                linearfb_put_pixel(xc - x, yc - y, color);
                linearfb_put_pixel(xc - y, yc - x, color);
                break;
            case 1: // Top-right
                linearfb_put_pixel(xc + x, yc - y, color);
                linearfb_put_pixel(xc + y, yc - x, color);
                break;
            case 2: // Bottom-left
                linearfb_put_pixel(xc - x, yc + y, color);
                linearfb_put_pixel(xc - y, yc + x, color);
                break;
            case 3: // Bottom-right
                linearfb_put_pixel(xc + x, yc + y, color);
                linearfb_put_pixel(xc + y, yc + x, color);
                break;
        }
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

static void linearfb_fill_corner(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color, int corner) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        switch (corner) {
            case 0: // Top-left
                linearfb_draw_line(xc - x, yc - y, xc, yc - y, color);
                linearfb_draw_line(xc - y, yc - x, xc, yc - x, color);
                break;
            case 1: // Top-right
                linearfb_draw_line(xc, yc - y, xc + x, yc - y, color);
                linearfb_draw_line(xc, yc - x, xc + y, yc - x, color);
                break;
            case 2: // Bottom-left
                linearfb_draw_line(xc - x, yc + y, xc, yc + y, color);
                linearfb_draw_line(xc - y, yc + x, xc, yc + x, color);
                break;
            case 3: // Bottom-right
                linearfb_draw_line(xc, yc + y, xc + x, yc + y, color);
                linearfb_draw_line(xc, yc + x, xc + y, yc + x, color);
                break;
        }
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

void linearfb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
    if (r == 0) { linearfb_draw_rect(x, y, w, h, color); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    linearfb_draw_line(x + r, y, x + w - r - 1, y, color);
    linearfb_draw_line(x + r, y + h - 1, x + w - r - 1, y + h - 1, color);
    linearfb_draw_line(x, y + r, x, y + h - r - 1, color);
    linearfb_draw_line(x + w - 1, y + r, x + w - 1, y + h - r - 1, color);

    linearfb_draw_corner(x + r, y + r, r, color, 0);
    linearfb_draw_corner(x + w - r - 1, y + r, r, color, 1);
    linearfb_draw_corner(x + r, y + h - r - 1, r, color, 2);
    linearfb_draw_corner(x + w - r - 1, y + h - r - 1, r, color, 3);
}
EXPORT_SYMBOL(linearfb_draw_rounded_rect);

void linearfb_fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
    if (r == 0) { linearfb_fill_rect(x, y, w, h, color); return; }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    linearfb_fill_rect(x + r, y, w - 2 * r, h, color);
    linearfb_fill_rect(x, y + r, r, h - 2 * r, color);
    linearfb_fill_rect(x + w - r, y + r, r, h - 2 * r, color);

    linearfb_fill_corner(x + r, y + r, r, color, 0);
    linearfb_fill_corner(x + w - r - 1, y + r, r, color, 1);
    linearfb_fill_corner(x + r, y + h - r - 1, r, color, 2);
    linearfb_fill_corner(x + w - r - 1, y + h - r - 1, r, color, 3);
}
EXPORT_SYMBOL(linearfb_fill_rounded_rect);

void linearfb_draw_shadow_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t opacity) {
    if (!fb) return;
    for (uint32_t i = 0; i < radius; i++) {
        uint8_t alpha = (uint8_t)((float)(radius - i) / radius * opacity);
        uint32_t color = linearfb_make_color_rgba(0, 0, 0, alpha);
        
        // Draw shadow edges with blending - slightly offset
        uint32_t sx = x + i, sy = y + i;
        // Right edge
        for (uint32_t r = 0; r < h; r++) linearfb_put_pixel_blend(x + w + i, y + i + r, color);
        // Bottom edge
        for (uint32_t c = 0; c < w; c++) linearfb_put_pixel_blend(x + i + c, y + h + i, color);
        // Corner
        linearfb_put_pixel_blend(x + w + i, y + h + i, color);
    }
}
EXPORT_SYMBOL(linearfb_draw_shadow_rect);

void linearfb_draw_text(const char *text, uint32_t x, uint32_t y, uint32_t color) {
    if (!text || !fb || !fb_font.data) return;
    uint32_t cx = x, cy = y;
    uint32_t stride = font_pitch ? font_pitch : (font_glyph_w + 7) / 8;
    uint32_t glyph_size = font_glyph_h * stride;

    while (*text) {
        char c = *text++;
        uint8_t ch = (uint8_t) c;
        if (ch >= font_glyph_count) ch = '?';
        const uint8_t *glyph = fb_font.data + ch * glyph_size;
        
        if (shadow_fb && fb->bpp == 32) {
            for (uint32_t r = 0; r < font_glyph_h; ++r) {
                const uint8_t *row_data = glyph + r * stride;
                uint32_t *sp = (uint32_t *)((uint8_t *)shadow_fb + (cy + r) * fb->pitch + cx * 4);
                uint32_t *vp = (uint32_t *)((uint8_t *)fb->address + (cy + r) * fb->pitch + cx * 4);
                
                for (uint32_t gx = 0; gx < font_glyph_w; ++gx) {
                    if (row_data[gx / 8] & (1 << (7 - (gx % 8)))) {
                        sp[gx] = color;
                    }
                }
                memcpy(vp, sp, font_glyph_w * 4);
            }
        } else {
            for (uint32_t r = 0; r < font_glyph_h; ++r) {
                const uint8_t *row_data = glyph + r * stride;
                for (uint32_t gx = 0; gx < font_glyph_w; ++gx) {
                    if (row_data[gx / 8] & (1 << (7 - (gx % 8)))) {
                        linearfb_put_pixel(cx + gx, cy + r, color);
                    }
                }
            }
        }
        cx += font_glyph_w;
    }
}
EXPORT_SYMBOL(linearfb_draw_text);

void linearfb_console_clear(uint32_t color) {
  if (!fb) return;
  if (fb->bpp == 32 && fb->pitch == fb->width * 4) {
      if (shadow_fb) memset32(shadow_fb, color, fb->width * fb->height);
      memset32(fb->address, color, fb->width * fb->height);
  } else {
      linearfb_fill_rect(0, 0, fb->width, fb->height, color);
  }
  memset(console_buffer, ' ', sizeof(console_buffer));
  console_col = 0;
  console_row = 0;
  console_bg = color;
}

static void linearfb_draw_glyph_at(uint32_t col, uint32_t row, char c) {
  if (!fb || !fb_font.data) return;
  if (col >= console_cols || row >= console_rows) return;

  uint32_t px = col * font_glyph_w;
  uint32_t py = row * font_glyph_h;
  uint8_t ch = (uint8_t) c;
  if (ch >= font_glyph_count) ch = '?';

  uint32_t stride = font_pitch ? font_pitch : (font_glyph_w + 7) / 8;
  const uint8_t *glyph = fb_font.data + ch * font_glyph_h * stride;

  if (shadow_fb && fb->bpp == 32) {
      for (uint32_t r = 0; r < font_glyph_h; ++r) {
        const uint8_t *row_data = glyph + r * stride;
        uint32_t *sp = (uint32_t *)((uint8_t *)shadow_fb + (py + r) * fb->pitch + px * 4);
        uint32_t *vp = (uint32_t *)((uint8_t *)fb->address + (py + r) * fb->pitch + px * 4);
        
        for (uint32_t cx = 0; cx < font_glyph_w; ++cx) {
          sp[cx] = (row_data[cx / 8] & (1 << (7 - (cx % 8)))) ? console_fg : console_bg;
        }
        // Blit line to VRAM
        memcpy(vp, sp, font_glyph_w * 4);
      }
  } else {
      for (uint32_t r = 0; r < font_glyph_h; ++r) {
        const uint8_t *row_data = glyph + r * stride;
        for (uint32_t cx = 0; cx < font_glyph_w; ++cx) {
          uint32_t color = (row_data[cx / 8] & (1 << (7 - (cx % 8)))) ? console_fg : console_bg;
          linearfb_put_pixel(px + cx, py + r, color);
        }
      }
  }
}

static void linearfb_draw_glyph_at_shadow(uint32_t col, uint32_t row, char c) {
  if (!shadow_fb || !fb_font.data) return;
  if (col >= console_cols || row >= console_rows) return;

  uint32_t px = col * font_glyph_w;
  uint32_t py = row * font_glyph_h;
  uint8_t ch = (uint8_t) c;
  if (ch >= font_glyph_count) ch = '?';

  uint32_t stride = font_pitch ? font_pitch : (font_glyph_w + 7) / 8;
  const uint8_t *glyph = fb_font.data + ch * font_glyph_h * stride;

  if (fb->bpp == 32) {
    for (uint32_t r = 0; r < font_glyph_h; ++r) {
      const uint8_t *row_data = glyph + r * stride;
      uint32_t *sp = (uint32_t *)((uint8_t *)shadow_fb + (py + r) * fb->pitch + px * 4);
      for (uint32_t cx = 0; cx < font_glyph_w; ++cx) {
        sp[cx] = (row_data[cx / 8] & (1 << (7 - (cx % 8)))) ? console_fg : console_bg;
      }
    }
  } else {
    // Fallback for non-32bpp
    for (uint32_t r = 0; r < font_glyph_h; ++r) {
      const uint8_t *row_data = glyph + r * stride;
      for (uint32_t gx = 0; gx < font_glyph_w; ++gx) {
        uint32_t color = (row_data[gx / 8] & (1 << (7 - (gx % 8)))) ? console_fg : console_bg;
        uint32_t x = px + gx;
        uint32_t y = py + r;
        uint8_t *sp = (uint8_t *)shadow_fb + y * fb->pitch + x * (fb->bpp / 8);
        memcpy(sp, &color, fb->bpp / 8);
      }
    }
  }
}

static void linearfb_console_redraw(void) {
  if (!fb) return;
  if (shadow_fb && fb->bpp == 32) {
      // Draw everything to shadow buffer first
      for (uint32_t y = 0; y < console_rows; ++y) {
        for (uint32_t x = 0; x < console_cols; ++x) {
          size_t idx = y * console_cols + x;
          if (idx < CONSOLE_BUF_MAX) {
            linearfb_draw_glyph_at_shadow(x, y, console_buffer[idx]);
          }
        }
      }
      // Then blit once
      memcpy(fb->address, shadow_fb, fb->height * fb->pitch);
  } else {
      for (uint32_t y = 0; y < console_rows; ++y) {
        for (uint32_t x = 0; x < console_cols; ++x) {
          size_t idx = y * console_cols + x;
          if (idx < CONSOLE_BUF_MAX) {
            linearfb_draw_glyph_at(x, y, console_buffer[idx]);
          }
        }
      }
  }
}

static void linearfb_console_scroll(void) {
  if (console_rows <= 1) return;
  size_t line_size = console_cols;
  size_t total_chars = console_rows * console_cols;
  if (total_chars > CONSOLE_BUF_MAX) total_chars = CONSOLE_BUF_MAX;
  size_t copy_size = (console_rows - 1) * line_size;
  
  if (copy_size < CONSOLE_BUF_MAX) {
    memmove(console_buffer, console_buffer + line_size, copy_size);
    memset(console_buffer + copy_size, ' ', line_size);
  }

  if (shadow_fb && fb->bpp == 32) {
      uint32_t font_h = font_glyph_h;
      uint32_t fb_pitch = fb->pitch;
      uint32_t fb_h = fb->height;

      // Scroll shadow buffer (RAM to RAM - extremely fast)
      memmove(shadow_fb, (uint8_t*)shadow_fb + font_h * fb_pitch, (fb_h - font_h) * fb_pitch);
      
      // Clear last line in shadow buffer
      for (uint32_t i = 0; i < font_h; i++) {
          uint32_t *line = (uint32_t *)((uint8_t *)shadow_fb + (fb_h - font_h + i) * fb_pitch);
          memset32(line, console_bg, fb->width);
      }

      // Blit entire screen to VRAM (ONE BIG WRITE - high throughput with WC)
      memcpy(fb->address, shadow_fb, fb_h * fb_pitch);
  } else {
    linearfb_console_redraw();
  }

  console_row = console_rows - 1;
  console_col = 0;
}

void linearfb_console_putc(char c) {
  irq_flags_t flags = spinlock_lock_irqsave(&fb_lock);
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
  size_t buf_idx = console_row * console_cols + console_col;
  if (buf_idx < CONSOLE_BUF_MAX) {
    console_buffer[buf_idx] = c;
  }
  linearfb_draw_glyph_at(console_col, console_row, c);
  if (++console_col >= console_cols) {
    console_col = 0;
    if (++console_row >= console_rows) linearfb_console_scroll();
  }
  spinlock_unlock_irqrestore(&fb_lock, flags);
}

void linearfb_console_puts(const char *s) {
  while (*s++) linearfb_console_putc(*s++);
}

int linearfb_load_font(const linearfb_font_t *font, const uint32_t count) {
  if (!font) return -1;
  irq_flags_t flags = spinlock_lock_irqsave(&fb_lock);
  fb_font = *font;
  font_glyph_w = font->width;
  font_glyph_h = font->height;
  font_pitch = font->pitch;
  font_glyph_count = count;
  if (fb && font_glyph_w && font_glyph_h) {
    console_cols = fb->width / font_glyph_w;
    console_rows = fb->height / font_glyph_h;
  }
  spinlock_unlock_irqrestore(&fb_lock, flags);
  return 0;
}
EXPORT_SYMBOL(linearfb_load_font);

static printk_backend_t fb_backend = {
  .name = "linearfb",
  .priority = 100,
  .putc = linearfb_console_putc,
  .probe = linearfb_probe,
  .init = linearfb_init_standard,
  .cleanup = linearfb_cleanup,
  .is_active = linearfb_is_initialized
};

const printk_backend_t *linearfb_get_backend(void) {
  return &fb_backend;
}

int linearfb_mod_init(void) {
  extern volatile struct limine_framebuffer_request *get_framebuffer_request(void);
  framebuffer_request = get_framebuffer_request();
  printk_register_backend(linearfb_get_backend());
  return 0;
}

FKX_MODULE_DEFINE(
  linearfb,
  "0.0.2",
  "assembler-0",
  "Linear Framebuffer Graphics Module",
  0,
  FKX_PRINTK_CLASS,
  linearfb_mod_init,
  NULL
);
