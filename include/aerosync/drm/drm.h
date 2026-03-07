/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/drm/drm.h
 * @brief DRM core interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <linux/list.h>
#include <aerosync/spinlock.h>
#include <aerosync//types.h>

struct drm_device;

struct drm_format_info {
    uint8_t bpp;
    uint8_t r_size, r_shift;
    uint8_t g_size, g_shift;
    uint8_t b_size, b_shift;
    uint8_t a_size, a_shift;
};

struct drm_framebuffer {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    void *vaddr;     /* Physical VRAM mapping */
    void *shadow;    /* Shadow buffer in main memory */
    size_t size;
    struct drm_format_info format;
};

struct drm_driver {
    const char *name;
    int (*load)(struct drm_device *dev);
    void (*unload)(struct drm_device *dev);
    void (*dirty_flush)(struct drm_device *dev, int x, int y, int w, int h);
};

struct drm_device {
    struct drm_driver *driver;
    void *dev_private;
    struct drm_framebuffer fb;
    struct list_head list;
    spinlock_t lock;
    struct char_device *cdev;
};

/**
 * @brief Register a DRM device.
 */
int drm_dev_register(struct drm_device *dev);

/**
 * @brief Unregister a DRM device.
 */
void drm_dev_unregister(struct drm_device *dev);

/**
 * @brief Get the primary DRM device.
 */
struct drm_device *drm_get_primary(void);

/**
 * @brief Initialize the simpledrm driver.
 */
int simpledrm_init(void);
