/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/graphics/drm/simpledrm/simpledrm.c
 * @brief Optimized Simple DRM driver for firmware-provided framebuffers
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/drm/drm.h>
#include <aerosync/errno.h>
#include <aerosync/fkx/fkx.h>
#include <arch/x86_64/requests.h>
#include <mm/slub.h>
#include <mm/vmalloc.h>
#include <arch/x86_64/mm/pmm.h>
#include <lib/string.h>

/**
 * @brief Optimized dirty rectangle flush using Write-Combining VRAM.
 */
static void simpledrm_dirty_flush(struct drm_device *dev, int x, int y, int w, int h) {
  struct drm_framebuffer *fb = &dev->fb;
  if (!fb->vaddr || !fb->shadow) return;

  /* Clamp coordinates to FB boundaries */
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x + w > (int) fb->width) w = fb->width - x;
  if (y + h > (int) fb->height) h = fb->height - y;
  if (w <= 0 || h <= 0) return;

  uint32_t bpp_bytes = fb->format.bpp / 8;
  size_t line_size = w * bpp_bytes;

  if (x == 0 && (uint32_t) w == fb->width && fb->pitch == (fb->width * bpp_bytes)) {
    memcpy((uint8_t *) fb->vaddr + y * fb->pitch, (uint8_t *) fb->shadow + y * fb->pitch, h * fb->pitch);
    return;
  }

  /* Otherwise, flush line by line */
  for (int i = 0; i < h; i++) {
    uint8_t *dst = (uint8_t *) fb->vaddr + (y + i) * fb->pitch + x * bpp_bytes;
    uint8_t *src = (uint8_t *) fb->shadow + (y + i) * fb->pitch + x * bpp_bytes;
    memcpy(dst, src, line_size);
  }
}

static struct drm_driver simpledrm_driver = {
  .name = "simpledrm",
  .dirty_flush = simpledrm_dirty_flush,
};

static void fill_format(volatile struct limine_framebuffer *lfb, struct drm_format_info *fmt) {
  fmt->bpp = lfb->bpp;
  fmt->r_size = lfb->red_mask_size;
  fmt->r_shift = lfb->red_mask_shift;
  fmt->g_size = lfb->green_mask_size;
  fmt->g_shift = lfb->green_mask_shift;
  fmt->b_size = lfb->blue_mask_size;
  fmt->b_shift = lfb->blue_mask_shift;

  uint32_t rgb_mask = ((1U << fmt->r_size) - 1) << fmt->r_shift |
                      ((1U << fmt->g_size) - 1) << fmt->g_shift |
                      ((1U << fmt->b_size) - 1) << fmt->b_shift;
  uint32_t alpha_mask = ~rgb_mask;
  if (alpha_mask && fmt->bpp == 32) {
    fmt->a_shift = __builtin_ctz(alpha_mask);
    fmt->a_size = __builtin_popcount(alpha_mask);
  } else {
    fmt->a_size = 0;
    fmt->a_shift = 0;
  }
}

int simpledrm_init(void) {
  volatile struct limine_framebuffer_request *req = get_framebuffer_request();
  if (!req || !req->response || req->response->framebuffer_count == 0) return -ENODEV;

  for (uint64_t i = 0; i < req->response->framebuffer_count; i++) {
    volatile struct limine_framebuffer *lfb = req->response->framebuffers[i];

    struct drm_device *dev = kzalloc(sizeof(struct drm_device));
    if (!dev) return -ENOMEM;

    dev->driver = &simpledrm_driver;
    dev->fb.width = lfb->width;
    dev->fb.height = lfb->height;
    dev->fb.pitch = lfb->pitch;
    dev->fb.size = lfb->height * lfb->pitch;
    fill_format(lfb, &dev->fb.format);

    uint64_t phys = pmm_virt_to_phys(lfb->address);
    dev->fb.vaddr = ioremap_wc(phys, dev->fb.size);
    if (!dev->fb.vaddr) {
      /* Fallback to Limine HHDM mapping if remapping fails */
      dev->fb.vaddr = lfb->address;
    }

    /* Allocate shadow buffer in standard WB RAM for fast reads/blending */
    dev->fb.shadow = vmalloc(dev->fb.size);
    if (dev->fb.shadow) {
      memset(dev->fb.shadow, 0, dev->fb.size);
    }

    spinlock_init(&dev->lock);
    drm_dev_register(dev);
  }

  return 0;
}
