#pragma once

#include <kernel/types.h>

// Flexible x86_64 GDT builder and loader

// Segment selectors (offsets into GDT). We provide stable indices for core segments.
#define GDT_NULL_INDEX          0
#define GDT_KERNEL_CODE_INDEX   1
#define GDT_KERNEL_DATA_INDEX   2
#define GDT_USER_CODE_INDEX     3
#define GDT_USER_DATA_INDEX     4

#define GDT_KERNEL_CODE_SEL   ((GDT_KERNEL_CODE_INDEX << 3) | 0)
#define GDT_KERNEL_DATA_SEL   ((GDT_KERNEL_DATA_INDEX << 3) | 0)
#define GDT_USER_CODE_SEL     ((GDT_USER_CODE_INDEX   << 3) | 3)
#define GDT_USER_DATA_SEL     ((GDT_USER_DATA_INDEX   << 3) | 3)

// Public API
void gdt_init(void);

// Optional accessors (future use)
static inline uint16_t gdt_kernel_cs(void) { return (uint16_t)GDT_KERNEL_CODE_SEL; }
static inline uint16_t gdt_kernel_ds(void) { return (uint16_t)GDT_KERNEL_DATA_SEL; }
static inline uint16_t gdt_user_cs(void)   { return (uint16_t)GDT_USER_CODE_SEL; }
static inline uint16_t gdt_user_ds(void)   { return (uint16_t)GDT_USER_DATA_SEL; }
