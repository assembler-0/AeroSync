/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/graphics/drm/linearfb/linearfb_internal.h
 * @brief Internal structures for linear framebuffer driver
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>
#include <aerosync/spinlock.h>
#include <aerosync/sysintf/char.h>
#include <limine/limine.h>
#include <lib/linearfb/linearfb.h>

struct linearfb_device {
  struct limine_framebuffer *limine_fb;
  void *vram;           /* Write-combined mapping of VRAM */
  void *shadow_fb;      /* Main memory copy for fast reads/blending */
  size_t size;
  struct char_device *cdev;

  /* Console state */
  uint32_t console_col, console_row;
  uint32_t console_cols, console_rows;
  uint32_t console_fg, console_bg;
  char *console_buffer;
  size_t console_buffer_size;

  /* Dirty tracking */
  uint32_t dirty_x0, dirty_y0, dirty_x1, dirty_y1;
  bool is_dirty;

  linearfb_color_format_t format;

  /* ANSI parsing state */
  enum {
    ANS_STATE_NORMAL,
    ANS_STATE_ESC,
    ANS_STATE_CSI,
  } ans_state;
  uint32_t ans_params[8];
  int ans_num_params;

  spinlock_t lock;
  struct list_head list;
};

extern struct list_head linearfb_devices;
extern struct linearfb_device *primary_fb;

/* Optimized internal primitives */
void linearfb_dev_put_pixel(struct linearfb_device *dev, uint32_t x, uint32_t y, uint32_t color);
void linearfb_dev_fill_rect(struct linearfb_device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void linearfb_dev_flush(struct linearfb_device *dev);
void linearfb_dev_scroll(struct linearfb_device *dev);
