/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/sysintf/fb.h
 * @brief Framebuffer class interface
 * @copyright (C) 2026 assembler-0
 */

#pragma once

#include <aerosync/sysintf/char.h>

struct char_device *fb_register_device(const struct char_operations *ops, void *private_data);
void fb_unregister_device(struct char_device *cdev);
