/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file include/aerosync/capability.h
 * @brief Kernel capability and security system
 * @copyright (C) 2025-2026 assembler-0
 */

#pragma once

#include <aerosync/types.h>

typedef uint64_t kernel_cap_t;

/* Scheduler Capabilities */
#define CAP_SYS_NICE     (1ULL << 0)  /* Allow raising priority/setting affinity */
#define CAP_SYS_RESOURCE (1ULL << 1)  /* Override resource limits */
#define CAP_SYS_ADMIN    (1ULL << 2)  /* General administrative rights */
#define CAP_SYS_BOOT     (1ULL << 3)  /* Allow boot-time operations */

/* VFS Capabilities */
#define CAP_CHOWN        (1ULL << 4)
#define CAP_DAC_OVERRIDE (1ULL << 5)
#define CAP_FOWNER       (1ULL << 6)

/* Network Capabilities */
#define CAP_NET_ADMIN    (1ULL << 7)
#define CAP_NET_RAW      (1ULL << 8)

struct task_struct;

/**
 * has_capability - Check if a task has a specific capability
 * @t: The task to check (nullptr for current)
 * @cap: The capability bit to check
 *
 * Returns true if the task possesses the capability.
 */
bool has_capability(struct task_struct *t, kernel_cap_t cap);

/**
 * capable - Check if current task has a capability
 */
static inline bool capable(kernel_cap_t cap) {
    return has_capability(nullptr, cap);
}
