/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/graphics/drm/font_manager.c
 * @brief Kernel font manager implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/font.h>
#include <aerosync/errno.h>
#include <lib/string.h>
#include <arch/x86_64/requests.h>

#define MAX_FONTS 16

static const struct font_desc *fonts[MAX_FONTS];
static int num_fonts = 0;

int font_register(const struct font_desc *font) {
  if (!font || num_fonts >= MAX_FONTS) return -ENOSPC;

  /* Check for duplicates */
  for (int i = 0; i < num_fonts; i++) {
    if (strcmp(fonts[i]->name, font->name) == 0) return -EEXIST;
  }

  fonts[num_fonts++] = font;
  return 0;
}

const struct font_desc *font_find(const char *name) {
  if (!name) return nullptr;
  for (int i = 0; i < num_fonts; i++) {
    if (strcmp(fonts[i]->name, name) == 0) return fonts[i];
  }
  return nullptr;
}

const struct font_desc *font_get_default(int xres, int yres) {
  if (num_fonts == 0) return nullptr;

  char cmdline_buff[64] = {0};
  cmdline_find_option(current_cmdline, "font", cmdline_buff, sizeof(cmdline_buff));

  const struct font_desc *best = nullptr;
  if (cmdline_buff[0]) best = font_find(cmdline_buff);

  if (best) return best;
  best = fonts[0];

  /* Heuristic: select largest font that fits reasonably */
  for (int i = 1; i < num_fonts; i++) {
    if (xres > 1024 && yres > 768) {
      if (fonts[i]->height > best->height) best = fonts[i];
    } else {
      /* Small screen, prefer smaller fonts or specific pref */
      if (fonts[i]->pref > best->pref) best = fonts[i];
    }
  }

  return best;
}
