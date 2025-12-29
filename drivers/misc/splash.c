/// SPDX-License-Identifier: GPL-2.0-only
/**
 * @file drivers/misc/splash.c
 * @brief Boot splash screen FKX module
 */

#include <kernel/classes.h>
#include <kernel/version.h>
#include <kernel/fkx/fkx.h>
#include <kernel/sched/process.h>
#include <lib/linearfb/linearfb.h>
#include <lib/printk.h>

static int splash_thread_fn(void *data) {
  (void) data;

  uint32_t width = 0, height = 0;
  linearfb_get_resolution(&width, &height);

  if (width == 0 || height == 0) {
    return -1;
  }

  // --- Theme: Pitch Black & Professional Accents ---
  uint32_t bg_color = 0xFF000000; // Pitch Black
  uint32_t primary_acc = linearfb_make_color(0, 120, 215);   // Windows-like Blue
  uint32_t secondary_acc = linearfb_make_color(0, 200, 255); // Brighter Blue
  uint32_t text_color = 0xFFFFFFFF; // White
  uint32_t shadow_color = 0x80000000; // Semi-transparent black for shadows

  // 1. Clear screen to pitch black
  linearfb_fill_rect(0, 0, width, height, bg_color);

  // 2. Center Card/Logo area
  uint32_t card_w = 400;
  uint32_t card_h = 220;
  uint32_t card_x = (width - card_w) / 2;
  uint32_t card_y = (height - card_h) / 2 - 20;

  // Draw shadow first
  linearfb_draw_shadow_rect(card_x + 5, card_y + 5, card_w, card_h, 15, 128);

  // Draw a sleek dark-grey rounded card with a gradient header
  uint32_t card_bg = linearfb_make_color(25, 25, 28);
  linearfb_fill_rounded_rect(card_x, card_y, card_w, card_h, 12, card_bg);
  
  // Header gradient
  linearfb_fill_rect_gradient(card_x + 2, card_y + 2, card_w - 4, 40, primary_acc, secondary_acc, 0);
  
  // "VoidFrameX" Title in the header
  linearfb_draw_text("VoidFrameX v" VOIDFRAMEX_VERSION_LEAN , card_x + 20, card_y + 12, text_color);
  
  // Subtitle / Version
  uint32_t dim_text = linearfb_color_brightness(text_color, 0.6f);
  linearfb_draw_text(KERN_CLASS "System Initialize...", card_x + 20, card_y + 60, dim_text);

  // 3. Fancy Progress Bar
  uint32_t bar_w = card_w - 60;
  uint32_t bar_h = 8;
  uint32_t bar_x = card_x + 30;
  uint32_t bar_y = card_y + 140;
  uint32_t bar_bg = linearfb_make_color(45, 45, 48);

  // Bar Background (rounded)
  linearfb_fill_rounded_rect(bar_x, bar_y, bar_w, bar_h, 4, bar_bg);

  const char *tasks[] = {
    KERN_CLASS "Loading kernel modules...",
    ACPI_CLASS "Initializing ACPI subsystem...",
    PCI_CLASS "Probing PCI bus devices...",
    VFS_CLASS "Mounting VFS root...",
    KERN_CLASS "Starting system services...",
    KERN_CLASS "Ready."
  };

  for (int i = 0; i <= 100; i++) {
    // Determine which task to show
    int task_idx = i / 20;
    if (task_idx > 5) task_idx = 5;
    
    // Clear task text area
    linearfb_fill_rect(card_x + 20, bar_y - 30, card_w - 40, 20, card_bg);
    linearfb_draw_text(tasks[task_idx], card_x + 30, bar_y - 25, secondary_acc);

    // Progress bar fill with glow effect (using blend)
    uint32_t progress = (bar_w * i) / 100;
    if (progress > 0) {
        // Draw the main progress bar
        linearfb_fill_rect_gradient(bar_x, bar_y, progress, bar_h, secondary_acc, primary_acc, 0);
        
        // Add a small "glow" at the tip using alpha blending
        if (progress < bar_w) {
            uint32_t glow_color = linearfb_make_color_rgba(0, 200, 255, 180);
            linearfb_fill_circle(bar_x + progress, bar_y + (bar_h / 2), 6, glow_color);
        }
    }

    delay_ms(40);
  }

  // 4. Final message
  uint32_t success_green = linearfb_make_color(100, 255, 100);
  linearfb_draw_text(KERN_CLASS "System operational.", card_x + 30, bar_y + 30, success_green);

  return 0;
}

int splash_mod_init(void) {
  // Use helpers to select a different backend for printk
  // We want to avoid using linearfb for printk so we can draw our splash
  const printk_backend_t *fallback = printk_auto_select_backend("linearfb");
  printk_set_sink(PRINTK_LOG_OR_NO_LOG(fallback), false);
  // Spawn a kernel thread for the splash screen
  struct task_struct *task = kthread_create(splash_thread_fn, NULL, "splash_screen");
  if (task) {
    kthread_run(task);
    return 0;
  }
  return -1;
}

const char *splash_deps[] = {"linearfb", NULL};

FKX_MODULE_DEFINE(
  splash,
  "0.1.0",
  "assembler-0",
  "System Splash Boot Screen",
  0,
  FKX_GENERIC_CLASS,
  splash_mod_init,
  splash_deps
);
