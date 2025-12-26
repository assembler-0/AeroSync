#pragma once

// Page size constants
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define PAGE_SIZE_2M (2UL * 1024 * 1024)
#define PAGE_SIZE_1G (1UL * 1024 * 1024 * 1024)

// Table Indices
#define PML5_INDEX(virt) (((virt) >> 48) & 0x1FF)
#define PML4_INDEX(virt) (((virt) >> 39) & 0x1FF)
#define PDPT_INDEX(virt) (((virt) >> 30) & 0x1FF)
#define PD_INDEX(virt)   (((virt) >> 21) & 0x1FF)
#define PT_INDEX(virt)   (((virt) >> 12) & 0x1FF)