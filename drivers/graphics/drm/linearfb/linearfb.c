/// SPDX-License-Identifier: GPL-2.0-only
/**
 * linearfb - Linear Framebuffer Driver
 *
 * @file drivers/graphics/drm/linearfb/linearfb.c
 * @brief sophisticated linear framebuffer graphics and console driver
 * @copyright (C) 2025-2026 assembler-0
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

#include "linearfb_internal.h"
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/char.h>
#include <aerosync/sysintf/fb.h>
#include <arch/x86_64/mm/pmm.h>
#include <arch/x86_64/requests.h>
#include <lib/linearfb/psf.h>
#include <lib/math.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <aerosync/fkx/fkx.h>
#include <mm/slub.h>
#include <mm/vmalloc.h>
#include <mm/vm_object.h>
#include <mm/vma.h>

extern const uint8_t embedded_console_font[];
extern const uint32_t embedded_console_font_size;

LIST_HEAD(linearfb_devices);
struct linearfb_device *primary_fb = nullptr;
static int fb_initialized = 0;

static linearfb_font_t fb_font = {0};
static uint32_t font_glyph_count = 0;

static volatile struct limine_framebuffer_request *framebuffer_request = nullptr;

/* --- Dirty Tracking --- */

static void linearfb_mark_dirty(struct linearfb_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  if (!dev) return;
  if (!dev->is_dirty) {
    dev->dirty_x0 = x;
    dev->dirty_y0 = y;
    dev->dirty_x1 = x + w;
    dev->dirty_y1 = y + h;
    dev->is_dirty = true;
  } else {
    dev->dirty_x0 = min(dev->dirty_x0, x);
    dev->dirty_y0 = min(dev->dirty_y0, y);
    dev->dirty_x1 = max(dev->dirty_x1, x + w);
    dev->dirty_y1 = max(dev->dirty_y1, y + h);
  }
}

void linearfb_dev_flush(struct linearfb_device *dev) {
  if (!dev || !dev->is_dirty || !dev->vram || !dev->shadow_fb) return;

  uint32_t x0 = dev->dirty_x0;
  uint32_t y0 = dev->dirty_y0;
  uint32_t x1 = min(dev->dirty_x1, dev->limine_fb->width);
  uint32_t y1 = min(dev->dirty_y1, dev->limine_fb->height);

  if (x1 <= x0 || y1 <= y0) {
    dev->is_dirty = false;
    return;
  }

  uint32_t bpp_bytes = dev->limine_fb->bpp / 8;
  uint32_t line_size = (x1 - x0) * bpp_bytes;

  for (uint32_t y = y0; y < y1; y++) {
    void *dst = (uint8_t *) dev->vram + y * dev->limine_fb->pitch + x0 * bpp_bytes;
    const void *src = (uint8_t *) dev->shadow_fb + y * dev->limine_fb->pitch + x0 * bpp_bytes;
    memcpy(dst, src, line_size);
  }

  dev->is_dirty = false;
}

/* --- Optimized Primitives --- */

void linearfb_dev_put_pixel(struct linearfb_device *dev, uint32_t x, uint32_t y, uint32_t color) {
  if (!dev || x >= dev->limine_fb->width || y >= dev->limine_fb->height) return;

  uint32_t bpp_bytes = dev->limine_fb->bpp / 8;
  uint8_t *p = (uint8_t *) dev->shadow_fb + y * dev->limine_fb->pitch + x * bpp_bytes;

  if (dev->limine_fb->bpp == 32) {
    *(uint32_t *) p = color;
  } else {
    memcpy(p, &color, bpp_bytes);
  }

  linearfb_mark_dirty(dev, x, y, 1, 1);
}

void linearfb_dev_fill_rect(struct linearfb_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  if (!dev) return;
  if (x >= dev->limine_fb->width || y >= dev->limine_fb->height) return;
  if (x + w > dev->limine_fb->width) w = dev->limine_fb->width - x;
  if (y + h > dev->limine_fb->height) h = dev->limine_fb->height - y;
  if (w == 0 || h == 0) return;

  uint32_t bpp_bytes = dev->limine_fb->bpp / 8;

  if (dev->limine_fb->bpp == 32) {
    for (uint32_t i = 0; i < h; i++) {
      uint32_t *p = (uint32_t *) ((uint8_t *) dev->shadow_fb + (y + i) * dev->limine_fb->pitch + x * 4);
      memset32(p, color, w);
    }
  } else {
    for (uint32_t i = 0; i < h; i++) {
      for (uint32_t j = 0; j < w; j++) {
        uint8_t *p = (uint8_t *) dev->shadow_fb + (y + i) * dev->limine_fb->pitch + (x + j) * bpp_bytes;
        memcpy(p, &color, bpp_bytes);
      }
    }
  }

  linearfb_mark_dirty(dev, x, y, w, h);
}

/* --- Character Device Ops --- */

static int linearfb_char_open(struct char_device *cdev) {
  (void) cdev;
  return 0;
}

static int linearfb_char_ioctl(struct char_device *cdev, uint32_t cmd, void *arg) {
  struct linearfb_device *dev = cdev->private_data;
  if (!dev) return -ENODEV;

  switch (cmd) {
    case 0x4600: { // FBIOGET_VSCREENINFO
      linearfb_surface_t *surf = (linearfb_surface_t *) arg;
      if (!access_ok(surf, sizeof(linearfb_surface_t))) return -EFAULT;
      
      linearfb_surface_t ksurf = {
        .address = dev->vram,
        .width = dev->limine_fb->width,
        .height = dev->limine_fb->height,
        .pitch = dev->limine_fb->pitch,
        .bpp = dev->limine_fb->bpp
      };
      
      if (copy_to_user(surf, &ksurf, sizeof(linearfb_surface_t)) != 0) return -EFAULT;
      return 0;
    }
    case 0x4601: { // FBIO_FLUSH
      linearfb_dev_flush(dev);
      return 0;
    }
  }
  return -EINVAL;
}

static int linearfb_char_mmap(struct char_device *cdev, struct vm_area_struct *vma) {
  struct linearfb_device *dev = cdev->private_data;
  if (!dev) return -ENODEV;

  size_t size = vma->vm_end - vma->vm_start;
  if (size > dev->size) return -EINVAL;

  uint64_t phys = pmm_virt_to_phys(dev->limine_fb->address);
  if (!phys) return -EFAULT;

  /* Create a physical memory VM object for the framebuffer */
  struct vm_object *obj = vm_object_device_create(phys, dev->size);
  if (!obj) return -ENOMEM;

  vma->vm_obj = obj;
  vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTCOPY | VM_DONTEXPAND | VM_CACHE_WC;
  vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);

  return 0;
}

static struct char_operations linearfb_char_ops = {
  .open = linearfb_char_open,
  .ioctl = linearfb_char_ioctl,
  .mmap = linearfb_char_mmap,
};

/* --- Console Implementation --- */

static void linearfb_dev_draw_glyph(struct linearfb_device *dev, uint32_t col, uint32_t row, char c) {
  if (!dev || !fb_font.data) return;
  if (col >= dev->console_cols || row >= dev->console_rows) return;

  uint32_t px = col * fb_font.width;
  uint32_t py = row * fb_font.height;
  uint8_t ch = (uint8_t) c;
  if (ch >= font_glyph_count) ch = '?';

  uint32_t stride = fb_font.pitch;
  const uint8_t *glyph = fb_font.data + ch * fb_font.height * stride;

  if (dev->limine_fb->bpp == 32) {
    for (uint32_t r = 0; r < fb_font.height; ++r) {
      const uint8_t *row_data = glyph + r * stride;
      uint32_t *sp = (uint32_t *) ((uint8_t *) dev->shadow_fb + (py + r) * dev->limine_fb->pitch + px * 4);
      for (uint32_t cx = 0; cx < fb_font.width; ++cx) {
        sp[cx] = (row_data[cx / 8] & (1 << (7 - (cx % 8)))) ? dev->console_fg : dev->console_bg;
      }
    }
  } else {
    /* Fallback for other BPPs */
    for (uint32_t r = 0; r < fb_font.height; ++r) {
      const uint8_t *row_data = glyph + r * stride;
      for (uint32_t cx = 0; cx < fb_font.width; ++cx) {
        uint32_t color = (row_data[cx / 8] & (1 << (7 - (cx % 8)))) ? dev->console_fg : dev->console_bg;
        linearfb_dev_put_pixel(dev, px + cx, py + r, color);
      }
    }
  }

  linearfb_mark_dirty(dev, px, py, fb_font.width, fb_font.height);
}

void linearfb_dev_scroll(struct linearfb_device *dev) {
  if (!dev || dev->console_rows <= 1) return;

  size_t line_chars = dev->console_cols;
  size_t copy_chars = (dev->console_rows - 1) * line_chars;

  if (dev->console_buffer) {
    memmove(dev->console_buffer, dev->console_buffer + line_chars, copy_chars);
    memset(dev->console_buffer + copy_chars, ' ', line_chars);
  }

  /* Scroll shadow buffer */
  uint32_t font_h = fb_font.height;
  uint32_t pitch = dev->limine_fb->pitch;
  uint32_t height = dev->limine_fb->height;

  memmove(dev->shadow_fb, (uint8_t *) dev->shadow_fb + font_h * pitch, (height - font_h) * pitch);

  /* Clear last line */
  for (uint32_t i = 0; i < font_h; i++) {
    void *line = (uint8_t *) dev->shadow_fb + (height - font_h + i) * pitch;
    if (dev->limine_fb->bpp == 32) {
      memset32(line, dev->console_bg, dev->limine_fb->width);
    } else {
      /* Slow fallback */
      for (uint32_t x = 0; x < dev->limine_fb->width; x++) {
        uint8_t *p = (uint8_t *) line + x * (dev->limine_fb->bpp / 8);
        memcpy(p, &dev->console_bg, dev->limine_fb->bpp / 8);
      }
    }
  }

  /* Full screen dirty */
  linearfb_mark_dirty(dev, 0, 0, dev->limine_fb->width, dev->limine_fb->height);
  linearfb_dev_flush(dev);

  dev->console_row = dev->console_rows - 1;
  dev->console_col = 0;
}

void linearfb_console_putc(char c) {
  if (!primary_fb) return;
  struct linearfb_device *dev = primary_fb;
  irq_flags_t flags = spinlock_lock_irqsave(&dev->lock);

  if (c == '\n') {
    dev->console_col = 0;
    if (++dev->console_row >= dev->console_rows) {
      linearfb_dev_scroll(dev);
    }
    spinlock_unlock_irqrestore(&dev->lock, flags);
    return;
  }

  if (c == '\r') {
    dev->console_col = 0;
    spinlock_unlock_irqrestore(&dev->lock, flags);
    return;
  }

  if (dev->console_buffer && dev->console_row * dev->console_cols + dev->console_col < dev->console_buffer_size) {
    dev->console_buffer[dev->console_row * dev->console_cols + dev->console_col] = c;
  }

  linearfb_dev_draw_glyph(dev, dev->console_col, dev->console_row, c);

  if (++dev->console_col >= dev->console_cols) {
    dev->console_col = 0;
    if (++dev->console_row >= dev->console_rows) {
      linearfb_dev_scroll(dev);
    }
  }

  /* Immediate flush for console output to ensure visibility during panic/boot */
  linearfb_dev_flush(dev);

  spinlock_unlock_irqrestore(&dev->lock, flags);
}

/* --- Public API Wrappers (for Primary FB) --- */

int linearfb_is_initialized(void) { return fb_initialized; }
int linearfb_probe(void) { return (framebuffer_request && framebuffer_request->response) ? 1 : 0; }

void linearfb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
  linearfb_dev_put_pixel(primary_fb, x, y, color);
}

uint32_t linearfb_get_pixel(uint32_t x, uint32_t y) {
  if (!primary_fb || x >= primary_fb->limine_fb->width || y >= primary_fb->limine_fb->height) return 0;
  uint32_t color = 0;
  uint32_t bpp_bytes = primary_fb->limine_fb->bpp / 8;
  const void *p = (uint8_t *) primary_fb->shadow_fb + y * primary_fb->limine_fb->pitch + x * bpp_bytes;
  memcpy(&color, p, bpp_bytes);
  return color;
}

void linearfb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  linearfb_dev_fill_rect(primary_fb, x, y, w, h, color);
}

void linearfb_console_clear(uint32_t color) {
  if (!primary_fb) return;
  linearfb_dev_fill_rect(primary_fb, 0, 0, primary_fb->limine_fb->width, primary_fb->limine_fb->height, color);
  if (primary_fb->console_buffer) memset(primary_fb->console_buffer, ' ', primary_fb->console_buffer_size);
  primary_fb->console_col = 0;
  primary_fb->console_row = 0;
  primary_fb->console_bg = color;
  linearfb_dev_flush(primary_fb);
}

/* --- Advanced Graphics Primitives --- */

void linearfb_draw_text(const char *text, uint32_t x, uint32_t y, uint32_t color) {
  if (!text || !primary_fb || !fb_font.data) return;
  uint32_t cx = x, cy = y;
  uint32_t stride = fb_font.pitch;
  uint32_t glyph_size = fb_font.height * stride;

  while (*text) {
    char c = *text++;
    uint8_t ch = (uint8_t) c;
    if (ch >= font_glyph_count) ch = '?';
    const uint8_t *glyph = fb_font.data + ch * glyph_size;

    if (primary_fb->limine_fb->bpp == 32) {
      for (uint32_t r = 0; r < fb_font.height; ++r) {
        if (cy + r >= primary_fb->limine_fb->height) break;
        const uint8_t *row_data = glyph + r * stride;
        uint32_t *sp = (uint32_t *) ((uint8_t *) primary_fb->shadow_fb + (cy + r) * primary_fb->limine_fb->pitch + cx * 4);

        for (uint32_t gx = 0; gx < fb_font.width; ++gx) {
          if (cx + gx >= primary_fb->limine_fb->width) break;
          if (row_data[gx / 8] & (1 << (7 - (gx % 8)))) {
            sp[gx] = color;
          }
        }
      }
    } else {
      for (uint32_t r = 0; r < fb_font.height; ++r) {
        const uint8_t *row_data = glyph + r * stride;
        for (uint32_t gx = 0; gx < fb_font.width; ++gx) {
          if (row_data[gx / 8] & (1 << (7 - (gx % 8)))) {
            linearfb_dev_put_pixel(primary_fb, cx + gx, cy + r, color);
          }
        }
      }
    }
    cx += fb_font.width;
  }
  linearfb_mark_dirty(primary_fb, x, y, cx - x, fb_font.height);
}

void linearfb_put_pixel_blend(uint32_t x, uint32_t y, uint32_t color) {
  if (!primary_fb || x >= primary_fb->limine_fb->width || y >= primary_fb->limine_fb->height) return;
  uint8_t alpha = (color >> 24) & 0xFF;
  if (alpha == 255) {
    linearfb_dev_put_pixel(primary_fb, x, y, color);
    return;
  }
  if (alpha == 0) return;

  uint32_t bg = linearfb_get_pixel(x, y);
  uint32_t r = (((color >> 16) & 0xFF) * alpha + ((bg >> 16) & 0xFF) * (255 - alpha)) >> 8;
  uint32_t g = (((color >> 8) & 0xFF) * alpha + ((bg >> 8) & 0xFF) * (255 - alpha)) >> 8;
  uint32_t b = ((color & 0xFF) * alpha + (bg & 0xFF) * (255 - alpha)) >> 8;

  linearfb_dev_put_pixel(primary_fb, x, y, (0xFFU << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b);
}

void linearfb_fill_rect_blend(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  for (uint32_t i = 0; i < h; i++) {
    for (uint32_t j = 0; j < w; j++) {
      linearfb_put_pixel_blend(x + j, y + i, color);
    }
  }
}

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

void linearfb_fill_rect_gradient(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c1, uint32_t c2, int vertical) {
  if (!primary_fb) return;
  if (vertical) {
    for (uint32_t i = 0; i < h; ++i) {
      uint32_t color = linearfb_color_lerp(c1, c2, (float) i / h);
      linearfb_dev_fill_rect(primary_fb, x, y + i, w, 1, color);
    }
  } else {
    for (uint32_t j = 0; j < w; ++j) {
      uint32_t color = linearfb_color_lerp(c1, c2, (float) j / w);
      linearfb_dev_fill_rect(primary_fb, x + j, y, 1, h, color);
    }
  }
}

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

uint32_t linearfb_color_brightness(uint32_t color, float amount) {
  uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF, a = (color >> 24) & 0xFF;
  r = clamp((int)(r * amount), 0, 255);
  g = clamp((int)(g * amount), 0, 255);
  b = clamp((int)(b * amount), 0, 255);
  return linearfb_make_color_rgba(r, g, b, a);
}

void linearfb_draw_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color) {
  int x = 0, y = r;
  int d = 3 - 2 * r;
  while (y >= x) {
    linearfb_dev_put_pixel(primary_fb, xc + x, yc + y, color);
    linearfb_dev_put_pixel(primary_fb, xc - x, yc + y, color);
    linearfb_dev_put_pixel(primary_fb, xc + x, yc - y, color);
    linearfb_dev_put_pixel(primary_fb, xc - x, yc - y, color);
    linearfb_dev_put_pixel(primary_fb, xc + y, yc + x, color);
    linearfb_dev_put_pixel(primary_fb, xc - y, yc + x, color);
    linearfb_dev_put_pixel(primary_fb, xc + y, yc - x, color);
    linearfb_dev_put_pixel(primary_fb, xc - y, yc - x, color);
    x++;
    if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
    else { d = d + 4 * x + 6; }
  }
}

void linearfb_fill_circle(uint32_t xc, uint32_t yc, uint32_t r, uint32_t color) {
  int x = 0, y = r;
  int d = 3 - 2 * r;
  while (y >= x) {
    linearfb_draw_line(xc - x, yc + y, xc + x, yc + y, color);
    linearfb_draw_line(xc - x, yc - y, xc + x, yc - y, color);
    linearfb_draw_line(xc - y, yc + x, xc + y, yc + x, color);
    linearfb_draw_line(xc - y, yc - x, xc + y, yc - x, color);
    x++;
    if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
    else { d = d + 4 * x + 6; }
  }
}

void linearfb_draw_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
  (void)r;
  linearfb_draw_rect(x, y, w, h, color);
}

void linearfb_fill_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t color) {
  (void)r;
  linearfb_fill_rect(x, y, w, h, color);
}

void linearfb_draw_shadow_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius, uint32_t opacity) {
  for (uint32_t i = 0; i < radius; i++) {
    uint8_t alpha = (uint8_t) ((float) (radius - i) / radius * opacity);
    uint32_t color = linearfb_make_color_rgba(0, 0, 0, alpha);
    for (uint32_t r = 0; r < h; r++) linearfb_put_pixel_blend(x + w + i, y + i + r, color);
    for (uint32_t c = 0; c < w; c++) linearfb_put_pixel_blend(x + i + c, y + h + i, color);
    linearfb_put_pixel_blend(x + w + i, y + h + i, color);
  }
}

void linearfb_draw_line_blend(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
  int dx = abs((int) x1 - (int) x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs((int) y1 - (int) y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  for (;;) {
    linearfb_put_pixel_blend(x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

/* --- Initialization --- */

static int linearfb_device_init(struct limine_framebuffer *lfb, int index) {
  (void)index;
  struct linearfb_device *dev = kzalloc(sizeof(struct linearfb_device));
  if (!dev) return -ENOMEM;

  dev->limine_fb = lfb;
  dev->size = lfb->height * lfb->pitch;

  /* Write-combining mapping for VRAM */
  uint64_t phys = pmm_virt_to_phys(lfb->address);
  if (phys) {
    dev->vram = ioremap_wc(phys, dev->size);
  } else {
    dev->vram = lfb->address;
  }

  /* Shadow framebuffer in main memory */
  dev->shadow_fb = vmalloc(dev->size);
  if (!dev->shadow_fb) {
    kfree(dev);
    return -ENOMEM;
  }
  memset(dev->shadow_fb, 0, dev->size);

  spinlock_init(&dev->lock);
  dev->console_fg = 0xFFFFFFFF;
  dev->console_bg = 0x00000000;

  if (fb_font.width && fb_font.height) {
    dev->console_cols = lfb->width / fb_font.width;
    dev->console_rows = lfb->height / fb_font.height;
    dev->console_buffer_size = dev->console_cols * dev->console_rows;
    dev->console_buffer = kmalloc(dev->console_buffer_size);
    if (dev->console_buffer) memset(dev->console_buffer, ' ', dev->console_buffer_size);
  }

  /* Register with UDM / FB class */
  dev->cdev = fb_register_device(&linearfb_char_ops, dev);
  if (!dev->cdev) {
    vfree(dev->shadow_fb);
    if (dev->console_buffer) kfree(dev->console_buffer);
    kfree(dev);
    return -ENODEV;
  }

  list_add_tail(&dev->list, &linearfb_devices);
  if (!primary_fb) primary_fb = dev;

  return 0;
}

int linearfb_init_standard(void *data) {
  (void)data;
  if (!framebuffer_request || !framebuffer_request->response) return -ENODEV;

  /* Load embedded font first */
  psf_font_t psf;
  if (psf_parse(embedded_console_font, embedded_console_font_size, &psf) == 0) {
    fb_font.width = psf.width;
    fb_font.height = psf.height;
    fb_font.data = psf.glyph_data;
    fb_font.pitch = psf.bytes_per_line;
    fb_font.bpp = 1;
    font_glyph_count = psf.num_glyphs;
  }

  for (uint64_t i = 0; i < framebuffer_request->response->framebuffer_count; i++) {
    linearfb_device_init(framebuffer_request->response->framebuffers[i], (int)i);
  }

  fb_initialized = 1;
  if (primary_fb) {
    linearfb_console_clear(0x00000000);
  }

  return primary_fb ? 0 : -1;
}

void linearfb_cleanup(void) {
  struct linearfb_device *dev, *tmp;
  list_for_each_entry_safe(dev, tmp, &linearfb_devices, list) {
    fb_unregister_device(dev->cdev);
    vfree(dev->shadow_fb);
    if (dev->console_buffer) kfree(dev->console_buffer);
    list_del(&dev->list);
    kfree(dev);
  }
  primary_fb = nullptr;
  fb_initialized = 0;
}

/* printk backend glue */
static printk_backend_t fb_backend = {
  .name = "linearfb",
  .priority = 100,
  .putc = linearfb_console_putc,
  .probe = linearfb_probe,
  .init = linearfb_init_standard,
  .cleanup = linearfb_cleanup,
  .is_active = linearfb_is_initialized
};

const printk_backend_t *linearfb_get_backend(void) { return &fb_backend; }

int __no_cfi linearfb_mod_init(void) {
  framebuffer_request = get_framebuffer_request();
  printk_register_backend(linearfb_get_backend());
  return 0;
}

/* Compatibility primitives */
void linearfb_draw_line(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t color) {
  int dx = abs((int)x1 - (int)x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs((int)y1 - (int)y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy, e2;
  for (;;) {
    linearfb_dev_put_pixel(primary_fb, x0, y0, color);
    if (x0 == x1 && y0 == y1) break;
    e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

void linearfb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
  linearfb_draw_line(x, y, x + w - 1, y, color);
  linearfb_draw_line(x, y + h - 1, x + w - 1, y + h - 1, color);
  linearfb_draw_line(x, y, x, y + h - 1, color);
  linearfb_draw_line(x + w - 1, y, x + w - 1, y + h - 1, color);
}

uint32_t linearfb_make_color(uint8_t r, uint8_t g, uint8_t b) { return (0xFFU << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b; }
uint32_t linearfb_make_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { return ((uint32_t) a << 24) | ((uint32_t) r << 16) | ((uint32_t) g << 8) | b; }

void linearfb_get_resolution(uint32_t *width, uint32_t *height) {
  if (primary_fb) {
    if (width) *width = primary_fb->limine_fb->width;
    if (height) *height = primary_fb->limine_fb->height;
  }
}

void linearfb_get_screen_surface(linearfb_surface_t *surface) {
  if (primary_fb && surface) {
    surface->address = primary_fb->vram;
    surface->width = primary_fb->limine_fb->width;
    surface->height = primary_fb->limine_fb->height;
    surface->pitch = primary_fb->limine_fb->pitch;
    surface->bpp = primary_fb->limine_fb->bpp;
  }
}

void linearfb_blit(linearfb_surface_t *dst, linearfb_surface_t *src, uint32_t dx, uint32_t dy, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h) {
  if (!dst || !src) return;
  uint32_t bpp_bytes = dst->bpp / 8;
  for (uint32_t i = 0; i < h; i++) {
    void *d = (uint8_t *)dst->address + (dy + i) * dst->pitch + dx * bpp_bytes;
    const void *s = (uint8_t *)src->address + (sy + i) * src->pitch + sx * bpp_bytes;
    memcpy(d, s, w * bpp_bytes);
  }
}

int linearfb_load_font(const linearfb_font_t* font, uint32_t count) {
  if (!font) return -EINVAL;
  fb_font = *font;
  font_glyph_count = count;
  return 0;
}

void linearfb_console_set_cursor(uint32_t col, uint32_t row) { if (primary_fb) { primary_fb->console_col = col; primary_fb->console_row = row; } }
void linearfb_console_get_cursor(uint32_t *col, uint32_t *row) { if (primary_fb) { if (col) *col = primary_fb->console_col; if (row) *row = primary_fb->console_row; } }
void linearfb_console_puts(const char *s) { while (*s) linearfb_console_putc(*s++); }

FKX_MODULE_DEFINE(linearfb, "0.1.0", "assembler-0", "Advanced Multi-FB Linear Framebuffer Driver", 0, FKX_PRINTK_CLASS, linearfb_mod_init, nullptr);

EXPORT_SYMBOL(linearfb_put_pixel);
EXPORT_SYMBOL(linearfb_fill_rect);
EXPORT_SYMBOL(linearfb_console_clear);
EXPORT_SYMBOL(linearfb_make_color);
EXPORT_SYMBOL(linearfb_make_color_rgba);
EXPORT_SYMBOL(linearfb_get_resolution);
EXPORT_SYMBOL(linearfb_get_screen_surface);
EXPORT_SYMBOL(linearfb_blit);
EXPORT_SYMBOL(linearfb_load_font);
EXPORT_SYMBOL(linearfb_draw_text);
EXPORT_SYMBOL(linearfb_put_pixel_blend);
EXPORT_SYMBOL(linearfb_fill_rect_blend);
EXPORT_SYMBOL(linearfb_draw_rect_blend);
EXPORT_SYMBOL(linearfb_fill_rect_gradient);
EXPORT_SYMBOL(linearfb_color_lerp);
EXPORT_SYMBOL(linearfb_color_brightness);
EXPORT_SYMBOL(linearfb_draw_circle);
EXPORT_SYMBOL(linearfb_fill_circle);
EXPORT_SYMBOL(linearfb_draw_rounded_rect);
EXPORT_SYMBOL(linearfb_fill_rounded_rect);
EXPORT_SYMBOL(linearfb_draw_shadow_rect);
EXPORT_SYMBOL(linearfb_draw_line_blend);
