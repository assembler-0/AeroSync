/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file drivers/fw/nvram.c
 * @brief NVRAM (CMOS) Manipulator Logic
 * @copyright (C) 2025-2026 assembler-0
 */

#include <aerosync/sysintf/fw.h>
#include <aerosync/sysintf/platform.h>
#include <aerosync/errno.h>
#include <arch/x86_64/io.h>
#include <aerosync/mutex.h>
#include <lib/printk.h>
#include <aerosync/classes.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static struct firmware_device nvram_fw_dev;
static mutex_t nvram_lock;

static uint8_t nvram_read(struct device *dev, uint16_t offset) {
  (void)dev;
  if (offset >= 128) return 0xFF;

  mutex_lock(&nvram_lock);
  outb(CMOS_ADDR, (uint8_t)offset);
  uint8_t val = inb(CMOS_DATA);
  mutex_unlock(&nvram_lock);
  return val;
}

static void nvram_write(struct device *dev, uint16_t offset, uint8_t val) {
  (void)dev;
  if (offset >= 128) return;

  mutex_lock(&nvram_lock);
  outb(CMOS_ADDR, (uint8_t)offset);
  outb(CMOS_DATA, val);
  mutex_unlock(&nvram_lock);
}

static size_t nvram_get_size(struct device *dev) {
  (void)dev;
  return 128;
}

static struct nvram_ops s_nvram_ops = {
  .read = nvram_read,
  .write = nvram_write,
  .get_size = nvram_get_size,
};

static int nvram_probe(struct platform_device *pdev) {
  struct firmware_device *fw_dev = container_of(pdev, struct firmware_device, pdev);
  fw_dev->ops = &s_nvram_ops;
  fw_dev->type = "nvram";

  mutex_init(&nvram_lock);
  printk(KERN_INFO NVRAM_CLASS "NVRAM driver initialized\n");

  // Smoke test: read CMOS time
  uint8_t secs = nvram_read(&pdev->dev, 0x00);
  uint8_t mins = nvram_read(&pdev->dev, 0x02);
  uint8_t hours = nvram_read(&pdev->dev, 0x04);
  printk(KERN_INFO NVRAM_CLASS "CMOS Time: %02x:%02x:%02x (BCD)\n", hours, mins, secs);

  return 0;
}

static struct platform_driver nvram_driver = {
  .probe = nvram_probe,
  .driver = {
    .name = "nvram",
  },
};

int nvram_init(void) {
  platform_driver_register(&nvram_driver);

  nvram_fw_dev.pdev.name = "nvram";
  nvram_fw_dev.pdev.id = -1;
  nvram_fw_dev.pdev.dev.class = &fw_class;

  return platform_device_register(&nvram_fw_dev.pdev);
}
