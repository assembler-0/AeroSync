#pragma once

#include <kernel/types.h>

/*
 * Vmalloc Direct Interface
 * Allocate virtually contiguous memory (physically non-contiguous).
 */
void *vmalloc(size_t size);
void vfree(void *ptr);
