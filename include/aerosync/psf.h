// SPDX-License-Identifier: GPL-2.0

#pragma once

#include <aerosync/types.h>

/**
 * PSF2 Header (from original psf.h)
 */
struct psf2_header {
  uint8_t magic[4];
  uint32_t version;
  uint32_t headersize;
  uint32_t flags;
  uint32_t length;
  uint32_t charsize;
  uint32_t height;
  uint32_t width;
};