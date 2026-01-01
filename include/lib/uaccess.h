#pragma once

#include <kernel/types.h>
#include <kernel/errno.h>

/**
 * copy_from_user - Copy a block of data from user space.
 * @to:   Destination address, in kernel space.
 * @from: Source address, in user space.
 * @n:    Number of bytes to copy.
 *
 * Context: User context only. This function may sleep if page faults occur.
 *
 * Returns number of bytes that could not be copied.
 * On success, this will be zero.
 */
size_t copy_from_user(void *to, const void *from, size_t n);

/**
 * copy_to_user - Copy a block of data into user space.
 * @to:   Destination address, in user space.
 * @from: Source address, in kernel space.
 * @n:    Number of bytes to copy.
 *
 * Context: User context only. This function may sleep if page faults occur.
 *
 * Returns number of bytes that could not be copied.
 * On success, this will be zero.
 */
size_t copy_to_user(void *to, const void *from, size_t n);

/**
 * access_ok - Checks if a user-space pointer is valid.
 */
static inline bool access_ok(const void *addr, size_t size) {
    uint64_t start = (uint64_t)addr;
    uint64_t end = start + size;

    // Check for overflow and ensure it's in the lower half of the address space
    // x86-64 canonical addresses: 0 to 0x00007FFFFFFFFFFF
    return (end >= start) && (end < 0x0000800000000000ULL);
}
