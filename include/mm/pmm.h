#pragma once

#include <kernel/types.h>
#include <pxs/protocol.h>

#define PAGE_SIZE 4096

void pmm_init(PXS_BOOT_INFO *boot_info);
void *pmm_alloc_page();
void *pmm_alloc_pages(size_t count);
void pmm_free_page(void *address);
void pmm_free_pages(void *address, size_t count);

// Debugging / Info
uint64_t pmm_get_total_memory();
uint64_t pmm_get_free_memory();
uint64_t pmm_get_used_memory();
