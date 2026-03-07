/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/font.h
 * @brief Kernel font manager interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>

/**
 * @brief Kernel font descriptor
 */
struct font_desc {
  const char *name;
  int width;
  int height;
  unsigned int charcount;
  const void *data;
  int pref; /* Preference/Priority for auto-selection */
};

/**
 * @brief Register a font with the manager.
 * @param font Pointer to the font descriptor.
 * @return 0 on success, negative error code otherwise.
 */
int font_register(const struct font_desc *font);

/**
 * @brief Find a font by name.
 * @param name Name of the font.
 * @return Pointer to the font descriptor, or NULL if not found.
 */
const struct font_desc *font_find(const char *name);

/**
 * @brief Get the best default font for a given resolution.
 * @param xres Horizontal resolution.
 * @param yres Vertical resolution.
 * @return Pointer to the font descriptor.
 */
const struct font_desc *font_get_default(int xres, int yres);

/**
 * @brief Register built-in fonts.
 */
void font_vga_8x16_register(void);
void font_sun12x22_register(void);
void font_sun8x16_register(void);

#define FONT_EXTRA_WORDS 4

#include <aerosync/compiler.h>

struct font_data {
  unsigned int extra[FONT_EXTRA_WORDS];
  const unsigned char data[];
} __packed;
