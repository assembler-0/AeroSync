/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/graphics/drm/drm_console.c
 * @brief Advanced DRM-based printk console and panic renderer
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/drm/drm.h>
#include <aerosync/font.h>
#include <aerosync/errno.h>
#include <aerosync/panic.h>
#include <aerosync/version.h>
#include <aerosync/ksymtab.h>
#include <aerosync/export.h>
#include <arch/x86_64/smp.h>
#include <lib/string.h>
#include <lib/log.h>

enum ansi_state {
  ANS_STATE_NORMAL,
  ANS_STATE_ESC,
  ANS_STATE_CSI
};

struct drm_console {
  struct drm_device *dev;
  const struct font_desc *font;
  uint32_t curr_col, curr_row;
  uint32_t cols, rows;
  uint32_t fg, bg;
  bool in_panic;

  enum ansi_state ansi_state;
  uint32_t ansi_params[8];
  int ansi_num_params;
};

static struct drm_console main_console;

static uint32_t ansi_colors[] = {
  0x00000000, 0x00AA0000, 0x0000AA00, 0x00AA5500,
  0x000000AA, 0x00AA00AA, 0x0000AAAA, 0x00AAAAAA,
  0x00555555, 0x00FF5555, 0x0055FF55, 0x00FFFF55,
  0x005555FF, 0x00FF55FF, 0x0055FFFF, 0x00FFFFFF,
};

/* --- Color & Pixel Helpers (BPP Generic) --- */

static uint32_t drm_encode_raw(struct drm_format_info *fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (!fmt) return ((uint32_t) a << 24) | (r << 16) | (g << 8) | b;
  uint32_t color = 0;
  color |= (uint32_t) ((r * ((1U << fmt->r_size) - 1) + 127) / 255) << fmt->r_shift;
  color |= (uint32_t) ((g * ((1U << fmt->g_size) - 1) + 127) / 255) << fmt->g_shift;
  color |= (uint32_t) ((b * ((1U << fmt->b_size) - 1) + 127) / 255) << fmt->b_shift;
  if (fmt->a_size) {
    color |= (uint32_t) ((a * ((1U << fmt->a_size) - 1) + 127) / 255) << fmt->a_shift;
  }
  return color;
}

static void drm_put_pixel(struct drm_device *dev, uint32_t x, uint32_t y, uint32_t raw_color) {
  void *target = main_console.in_panic ? dev->fb.vaddr : dev->fb.shadow;
  if (!target || x >= dev->fb.width || y >= dev->fb.height) return;

  uint8_t *dst = (uint8_t *) target + y * dev->fb.pitch + x * (dev->fb.format.bpp / 8);

  switch (dev->fb.format.bpp) {
    case 32:
      *(uint32_t *) dst = raw_color;
      break;
    case 24:
      dst[0] = (raw_color >> 0) & 0xFF;
      dst[1] = (raw_color >> 8) & 0xFF;
      dst[2] = (raw_color >> 16) & 0xFF;
      break;
    case 16:
      *(uint16_t *) dst = (uint16_t) raw_color;
      break;
    case 8:
      *dst = (uint8_t) raw_color;
      break;
  }
}

static uint32_t drm_get_pixel(struct drm_device *dev, uint32_t x, uint32_t y) {
  void *src = main_console.in_panic ? dev->fb.vaddr : dev->fb.shadow;
  if (!src || x >= dev->fb.width || y >= dev->fb.height) return 0;

  uint8_t *p = (uint8_t *) src + y * dev->fb.pitch + x * (dev->fb.format.bpp / 8);

  switch (dev->fb.format.bpp) {
    case 32:
      return *(uint32_t *) p;
    case 24:
      return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16);
    case 16:
      return *(uint16_t *) p;
    case 8:
      return *p;
  }
  return 0;
}

static void drm_decode_raw(struct drm_format_info *fmt, uint32_t raw, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
  if (!fmt) {
    *a = (raw >> 24) & 0xFF;
    *r = (raw >> 16) & 0xFF;
    *g = (raw >> 8) & 0xFF;
    *b = raw & 0xFF;
    return;
  }
  *r = (((raw >> fmt->r_shift) & ((1U << fmt->r_size) - 1)) * 255 + ((1U << fmt->r_size) / 2)) /
       ((1U << fmt->r_size) - 1);
  *g = (((raw >> fmt->g_shift) & ((1U << fmt->g_size) - 1)) * 255 + ((1U << fmt->g_size) / 2)) /
       ((1U << fmt->g_size) - 1);
  *b = (((raw >> fmt->b_shift) & ((1U << fmt->b_size) - 1)) * 255 + ((1U << fmt->b_size) / 2)) /
       ((1U << fmt->b_size) - 1);
  if (fmt->a_size)
    *a = (((raw >> fmt->a_shift) & ((1U << fmt->a_size) - 1)) * 255 + ((1U << fmt->a_size) / 2)) /
         ((1U << fmt->a_size) - 1);
  else
    *a = 255;
}

static void drm_put_pixel_blend(struct drm_device *dev, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b,
                                uint8_t a) {
  if (a == 0) return;
  if (a == 255) {
    drm_put_pixel(dev, x, y, drm_encode_raw(&dev->fb.format, r, g, b, a));
    return;
  }

  uint32_t bg_raw = drm_get_pixel(dev, x, y);
  uint8_t br, bg_g, bb, ba;
  drm_decode_raw(&dev->fb.format, bg_raw, &br, &bg_g, &bb, &ba);

  uint8_t nr = (r * a + br * (255 - a)) / 255;
  uint8_t ng = (g * a + bg_g * (255 - a)) / 255;
  uint8_t nb = (b * a + bb * (255 - a)) / 255;

  drm_put_pixel(dev, x, y, drm_encode_raw(&dev->fb.format, nr, ng, nb, 255));
}

/* --- Graphics Primitives --- */

static void drm_fill_rect(struct drm_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t raw_color) {
  for (uint32_t i = 0; i < h; i++) {
    for (uint32_t j = 0; j < w; j++) {
      drm_put_pixel(dev, x + j, y + i, raw_color);
    }
  }
}

static void drm_draw_shadow_rect(struct drm_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t radius,
                                 uint8_t opacity) {
  for (uint32_t i = 0; i < radius; i++) {
    for (uint32_t j = radius; j < h + radius; j++)
      drm_put_pixel_blend(dev, x + w + i, y + j, 0, 0, 0, opacity);
    for (uint32_t j = radius; j < w; j++)
      drm_put_pixel_blend(dev, x + j, y + h + i, 0, 0, 0, opacity);
  }
}

static void drm_fill_rounded_rect(struct drm_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r,
                                  uint32_t raw_color) {
  /* Simplified rounded rect for kernel console: just rect for now */
  drm_fill_rect(dev, x, y, w, h, raw_color);
}

/* --- Console Core --- */

static void drm_console_draw_glyph(struct drm_console *con, uint32_t col, uint32_t row, char c) {
  struct drm_device *dev = con->dev;
  const struct font_desc *font = con->font;
  if (col >= con->cols || row >= con->rows) return;

  uint32_t px = col * font->width;
  uint32_t py = row * font->height;
  uint32_t bytes_per_line = (font->width + 7) / 8;
  const uint8_t *glyph = (const uint8_t *) font->data + (uint8_t) c * font->height * bytes_per_line;

  for (int y = 0; y < font->height; y++) {
    for (int x = 0; x < font->width; x++) {
      uint32_t color = (glyph[y * bytes_per_line + x / 8] & (1 << (7 - (x % 8)))) ? con->fg : con->bg;
      drm_put_pixel(dev, px + x, py + y, color);
    }
  }

  if (!con->in_panic && dev->driver->dirty_flush) {
    dev->driver->dirty_flush(dev, (int)px, (int)py, font->width, font->height);
  }
}

static void drm_console_scroll(struct drm_console *con) {
  struct drm_device *dev = con->dev;
  uint32_t font_h = con->font->height;
  void *target = con->in_panic ? dev->fb.vaddr : dev->fb.shadow;
  if (!target) return;

  memmove(target, (uint8_t *) target + font_h * dev->fb.pitch, (dev->fb.height - font_h) * dev->fb.pitch);

  /* Clear last line */
  uint32_t last_line_y = dev->fb.height - font_h;
  for (uint32_t y = 0; y < font_h; y++) {
    for (uint32_t x = 0; x < dev->fb.width; x++) {
      drm_put_pixel(dev, x, last_line_y + y, con->bg);
    }
  }

  if (!con->in_panic && dev->driver->dirty_flush) {
    dev->driver->dirty_flush(dev, 0, 0, (int)dev->fb.width, (int)dev->fb.height);
  }
  con->curr_row = con->rows - 1;
  con->curr_col = 0;
}

static void drm_ansi_apply(struct drm_console *con) {
  if (con->ansi_num_params == 0) {
    /* Default to white on black if no params provided (\e[m) */
    con->fg = drm_encode_raw(&con->dev->fb.format, 255, 255, 255, 255);
    con->bg = drm_encode_raw(&con->dev->fb.format, 0, 0, 0, 255);
    return;
  }

  for (int i = 0; i < con->ansi_num_params; i++) {
    uint32_t p = con->ansi_params[i];
    if (p == 0) {
      con->fg = drm_encode_raw(&con->dev->fb.format, 255, 255, 255, 255);
      con->bg = drm_encode_raw(&con->dev->fb.format, 0, 0, 0, 255);
    } else if (p >= 30 && p <= 37) {
      uint32_t c = ansi_colors[p - 30];
      con->fg = drm_encode_raw(&con->dev->fb.format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    } else if (p >= 90 && p <= 97) { /* Bright colors */
      uint32_t c = ansi_colors[p - 90 + 8];
      con->fg = drm_encode_raw(&con->dev->fb.format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    } else if (p >= 40 && p <= 47) {
      uint32_t c = ansi_colors[p - 40];
      con->bg = drm_encode_raw(&con->dev->fb.format, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, 255);
    }
  }
}

void drm_console_putc(char c) {
  struct drm_console *con = &main_console;
  if (!con->dev) return;

  if (con->ansi_state == ANS_STATE_ESC) {
    if (c == '[') {
      con->ansi_state = ANS_STATE_CSI;
      con->ansi_num_params = 0;
      con->ansi_params[0] = 0;
      return;
    }
    con->ansi_state = ANS_STATE_NORMAL;
  } else if (con->ansi_state == ANS_STATE_CSI) {
    if (c >= '0' && c <= '9') {
      con->ansi_params[con->ansi_num_params] = con->ansi_params[con->ansi_num_params] * 10 + (c - '0');
      return;
    } else if (c == ';') {
      if (con->ansi_num_params < 7) {
        con->ansi_num_params++;
        con->ansi_params[con->ansi_num_params] = 0;
      }
      return;
    } else if (c == 'm') {
      con->ansi_num_params++;
      drm_ansi_apply(con);
      con->ansi_state = ANS_STATE_NORMAL;
      return;
    }
    con->ansi_state = ANS_STATE_NORMAL;
    return;
  }

  if (c == '\033') {
    con->ansi_state = ANS_STATE_ESC;
    return;
  }
  if (c == '\n') {
    con->curr_col = 0;
    if (++con->curr_row >= con->rows) drm_console_scroll(con);
    return;
  }
  if (c == '\r') {
    con->curr_col = 0;
    return;
  }

  drm_console_draw_glyph(con, con->curr_col, con->curr_row, c);
  if (++con->curr_col >= con->cols) {
    con->curr_col = 0;
    if (++con->curr_row >= con->rows) drm_console_scroll(con);
  }
}

/* --- Panic Support --- */

static void drm_panic_printf_at(uint32_t x, uint32_t y, uint32_t color, const char *fmt, ...) {
  struct drm_console *con = &main_console;
  va_list args;
  va_start(args, fmt);
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  char *p = buf;
  uint32_t cx = x;
  uint32_t bytes_per_line = (con->font->width + 7) / 8;

  while (*p) {
    uint8_t c = (uint8_t) *p++;
    const uint8_t *glyph = (const uint8_t *) con->font->data + c * con->font->height * bytes_per_line;
    for (int r = 0; r < con->font->height; r++) {
      for (int k = 0; k < con->font->width; k++) {
        if (glyph[r * bytes_per_line + k / 8] & (1 << (7 - (k % 8))))
          drm_put_pixel(con->dev, cx + k, y + r, color);
      }
    }
    cx += con->font->width;
  }
}

static void fb_panic_dump_stack(uint32_t x, uint32_t y, uint64_t rbp, uint64_t rip, uint32_t clr_addr,
                                uint32_t clr_sym) {
  uintptr_t *frame = (uintptr_t *) rbp;
  int depth = 0;
  uint32_t cy = y;
  struct drm_console *con = &main_console;

  if (rip) {
    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(rip, &offset);
    drm_panic_printf_at(x, cy, clr_addr, "[<%016llx>] ", rip);
    if (name) drm_panic_printf_at(x + 168, cy, clr_sym, "%s+0x%lx", name, offset);
    cy += con->font->height + 2;
  }

  while (depth < 16) {
    if ((uintptr_t) frame < 0xFFFF800000000000ULL || ((uintptr_t) frame & 0x7)) break;
    uintptr_t ret_addr = frame[1];
    if (!ret_addr) break;
    uintptr_t offset = 0;
    const char *name = lookup_ksymbol_by_addr(ret_addr, &offset);
    drm_panic_printf_at(x, cy, clr_addr, "[<%016lx>] ", ret_addr);
    if (name) drm_panic_printf_at(x + 168, cy, clr_sym, "%s+0x%lx", name, offset);
    cy += con->font->height + 2;
    uintptr_t next_rbp = frame[0];
    if (next_rbp <= (uintptr_t) frame) break;
    frame = (uintptr_t *) next_rbp;
    depth++;
  }
}

static void drm_panic_render(const char *reason, cpu_regs *regs) {
  struct drm_console *con = &main_console;
  struct drm_device *dev = con->dev;
  if (!dev) return;

  con->in_panic = true;

  uint32_t clr_box_bg = drm_encode_raw(&dev->fb.format, 8, 8, 12, 255);
  uint32_t clr_accent = drm_encode_raw(&dev->fb.format, 255, 40, 40, 255);
  uint32_t clr_header = drm_encode_raw(&dev->fb.format, 240, 240, 250, 255);
  uint32_t clr_subtext = drm_encode_raw(&dev->fb.format, 120, 120, 140, 255);
  uint32_t clr_link = drm_encode_raw(&dev->fb.format, 60, 150, 255, 255);
  uint32_t clr_reg_label = drm_encode_raw(&dev->fb.format, 100, 160, 230, 255);
  uint32_t clr_reg_val = drm_encode_raw(&dev->fb.format, 220, 220, 230, 255);
  uint32_t clr_stack_addr = drm_encode_raw(&dev->fb.format, 110, 110, 130, 255);
  uint32_t clr_stack_sym = drm_encode_raw(&dev->fb.format, 240, 190, 100, 255);

  /* Background Gradient */
  for (uint32_t i = 0; i < dev->fb.height; i++) {
    uint8_t r = (uint8_t) (2 + (0 - 2) * (int) i / (int) dev->fb.height);
    uint8_t g = (uint8_t) (2 + (0 - 2) * (int) i / (int) dev->fb.height);
    uint8_t b = (uint8_t) (6 + (0 - 6) * (int) i / (int) dev->fb.height);
    drm_fill_rect(dev, 0, i, dev->fb.width, 1, drm_encode_raw(&dev->fb.format, r, g, b, 255));
  }

  uint32_t pad = 60;
  uint32_t x = pad, y = pad;

  /* Header */
  drm_draw_shadow_rect(dev, x - 5, y - 5, dev->fb.width - 2 * pad + 10, 90, 15, 80);
  drm_fill_rounded_rect(dev, x, y, dev->fb.width - 2 * pad, 80, 8, clr_box_bg);
  drm_panic_printf_at(x + 30, y + 20, clr_accent, "GSF - Global System Failure");
  drm_panic_printf_at(x + 30, y + 45, clr_header, "AeroSync %s", AEROSYNC_VERSION_LEAN);

  y += 110;
  drm_fill_rounded_rect(dev, x, y, dev->fb.width - 2 * pad, 50, 4, drm_encode_raw(&dev->fb.format, 35, 20, 20, 255));
  drm_panic_printf_at(x + 20, y + 18, clr_accent, "STOP_CODE: %s", reason);

  y += 80;
  uint32_t col_w = (dev->fb.width - 2 * pad - 40) / 2;
  drm_fill_rounded_rect(dev, x, y, col_w, dev->fb.height - y - pad - 60, 6, clr_box_bg);
  if (regs) {
    uint32_t rx = x + 20, ry = y + 40;
    const char *names[] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP", "R8 ", "R9 ", "R10", "R11", "R12",
                           "R13", "R14", "R15", "RIP", "FLG"};
    uint64_t vals[] = {regs->rax, regs->rbx, regs->rcx, regs->rdx, regs->rsi, regs->rdi, regs->rbp, regs->rsp, regs->r8,
                       regs->r9, regs->r10, regs->r11, regs->r12, regs->r13, regs->r14, regs->r15, regs->rip,
                       regs->rflags};
    for (int i = 0; i < 18; i++) {
      drm_panic_printf_at(rx, ry, clr_reg_label, "%s", names[i]);
      drm_panic_printf_at(rx + 40, ry, clr_reg_val, "%016llx", vals[i]);
      ry += con->font->height + 4;
      if (ry > dev->fb.height - pad - 80) break;
    }
  }

  uint32_t tx = x + col_w + 40;
  drm_fill_rounded_rect(dev, tx, y, col_w, dev->fb.height - y - pad - 60, 6, clr_box_bg);
  fb_panic_dump_stack(tx + 20, y + 40, regs ? regs->rbp : (uint64_t) __builtin_frame_address(0), regs ? regs->rip : 0,
                      clr_stack_addr, clr_stack_sym);

  y = dev->fb.height - pad - 30;
  drm_panic_printf_at(x, y, clr_subtext, "System halted. Report issues at ");
  drm_panic_printf_at(x + 260, y, clr_link, "https://github.com/assembler-0/AeroSync/issues");
}

/* --- Backend Integration --- */

static void __exit __noinline __sysv_abi drm_panic_handler(const char *msg) {
  drm_panic_render(msg, nullptr);
}

static void __exit __noinline __sysv_abi drm_panic_exception(cpu_regs *regs) {
  char reason[256];
  snprintf(reason, sizeof(reason), "Exception 0x%llx, Error Code: 0x%llx", regs->interrupt_number, regs->error_code);
  drm_panic_render(reason, regs);
}

static const panic_ops_t drm_panic_ops = {
  .name = "drm_panic",
  .prio = 10,
  .panic = drm_panic_handler,
  .panic_exception = drm_panic_exception,
};

static void drm_backend_write(const char *buf, size_t len, int level) {
  if (len == 0) return;
  const char *ansi = klog_level_to_ansi(level);
  if (*ansi) while (*ansi) drm_console_putc(*ansi++);
  for (size_t i = 0; i < len; i++) drm_console_putc(buf[i]);
  if (*ansi) {
    const char *reset = ANS_RESET;
    while (*reset) drm_console_putc(*reset++);
  }
}

static int drm_probe(void) {
  return !!drm_get_primary();
}

static int drm_console_init(void *payload) {
  (void) payload;
  struct drm_device *dev = drm_get_primary();
  if (!dev) return -ENODEV;

  /* Rationale: Prevent GPF if dev is partially initialized/poisoned */
  if (unlikely(!dev->driver || !dev->fb.vaddr || !dev->fb.width || !dev->fb.height || !dev->fb.format.bpp)) {
    return -EAGAIN;
  }

  main_console.dev = dev;
  main_console.font = font_get_default((int)dev->fb.width, (int)dev->fb.height);
  if (!main_console.font) return -ENODEV;

  main_console.cols = dev->fb.width / main_console.font->width;
  main_console.rows = dev->fb.height / main_console.font->height;
  main_console.fg = drm_encode_raw(&dev->fb.format, 255, 255, 255, 255);
  main_console.bg = drm_encode_raw(&dev->fb.format, 0, 0, 0, 255);

  panic_register_handler(&drm_panic_ops);
  printk(KERN_DEBUG DRM_CLASS "drm_console initialized (%dx%d)\n", dev->fb.width, dev->fb.height);
  return 0;
}

static printk_backend_t drm_backend = {
  .name = "drm",
  .priority = 100,
  .write = drm_backend_write,
  .probe = drm_probe,
  .init = drm_console_init
};

void drm_console_signal_ready(void) {
  printk_backend_signal_ready(&drm_backend);
}
EXPORT_SYMBOL(drm_console_signal_ready);

int drm_console_init_default(void) {
  printk_register_backend_pending(&drm_backend);
  return 0;
}
