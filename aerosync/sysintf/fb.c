/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file aerosync/sysintf/fb.c
 * @brief Framebuffer class implementation
 * @copyright (C) 2026 assembler-0
 */

#include <aerosync/sysintf/fb.h>
#include <aerosync/sysintf/class.h>
#include <aerosync/export.h>
#include <mm/slub.h>

static struct class fb_class = {
  .name = "graphics",
  .dev_prefix = CONFIG_FB_NAME_PREFIX,
  .naming_scheme = NAMING_NUMERIC,
  .category = DEV_CAT_FB,
  .flags = CLASS_FLAG_AUTO_DEVTMPFS,
};

static struct device_driver fb_driver = {
  .name = "fb_core",
};

struct char_device *fb_register_device(const struct char_operations *ops, void *private_data) {
  static int initialized = 0;
  if (!initialized) {
    class_register(&fb_class);
    initialized = 1;
  }

  struct char_device *cdev = kzalloc(sizeof(struct char_device));
  if (!cdev) return nullptr;

  cdev->dev.class = &fb_class;
  cdev->dev.driver = &fb_driver;
  cdev->ops = ops;
  cdev->private_data = private_data;

  /* FB major 29, minors via IDA */
  int id = ida_alloc(&fb_class.ida, GFP_KERNEL);
  if (id < 0) {
    kfree(cdev);
    return nullptr;
  }
  cdev->dev.id = id;
  cdev->dev_num = MKDEV(29, id);

  if (char_device_register(cdev) != 0) {
    ida_free(&fb_class.ida, id);
    kfree(cdev);
    return nullptr;
  }

  return cdev;
}
EXPORT_SYMBOL(fb_register_device);

void fb_unregister_device(struct char_device *cdev) {
  if (!cdev) return;

  char_device_unregister(cdev);
  ida_free(&fb_class.ida, cdev->dev.id);
  kfree(cdev);
}
EXPORT_SYMBOL(fb_unregister_device);