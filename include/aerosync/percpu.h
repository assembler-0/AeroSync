/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#include <aerosync/types.h>
#include <arch/x86_64/percpu.h>

/**
 * alloc_percpu - Allocate dynamic per-CPU memory
 * @size: Size of the allocation
 * @align: Alignment requirement
 *
 * Returns a pointer that can be used with this_cpu_ptr() or per_cpu_ptr().
 * The returned pointer is actually an offset relative to the per-CPU base.
 */
void *pcpu_alloc(size_t size, size_t align);

/**
 * free_percpu - Free dynamic per-CPU memory
 * @ptr: Pointer returned by pcpu_alloc()
 */
void pcpu_free(void *ptr);

#define alloc_percpu(type) \
    (typeof(type) __percpu *)pcpu_alloc(sizeof(type), __alignof__(type))

#define free_percpu(ptr) \
    pcpu_free((void *)(ptr))

void percpu_test(void);

/*
 * Note: this_cpu_ptr and per_cpu_ptr are already defined in arch/x86_64/percpu.h
 * and they work by adding __per_cpu_offset[cpu] to the address.
 * Our pcpu_alloc will return (void*)((unsigned long)_percpu_start + offset), 
 * so that adding __per_cpu_offset[cpu] lands in the right spot.
 *
 * __per_cpu_offset[cpu] = ptr_cpu - _percpu_start
 * target = ((_percpu_start + offset) + (ptr_cpu - _percpu_start)) = ptr_cpu + offset.
 * PERFECT.
 */
