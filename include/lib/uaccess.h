#pragma once

#include <arch/x86_64/mm/vmm.h>
#include <aerosync/types.h>
#include <aerosync/compiler.h>

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
size_t copy_from_user(void *to, const void __user *from, size_t n);

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
size_t copy_to_user(void __user *to, const void *from, size_t n);

/**
 * access_ok - Checks if a user-space pointer is valid.
 */
static inline bool access_ok(const void __user *addr, size_t size) {
    uint64_t start = (uint64_t)addr;
    uint64_t end = start + size;
    uint64_t limit = vmm_get_max_user_address();

    // Check for overflow and ensure it's within the valid user address range.
    // This dynamically handles both 4-level (48-bit) and 5-level (57-bit) paging.
    return (end >= start) && (end < limit);
}
