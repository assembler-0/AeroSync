/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/drm/drm_console.h
 * @brief DRM-based printk console interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

/**
 * @brief Initialize the default DRM console.
 * @return 0 on success, negative error code otherwise.
 */
int drm_console_init_default(void);

/**
 * @brief Signal that the primary DRM device is ready for console output.
 */
void drm_console_signal_ready(void);
