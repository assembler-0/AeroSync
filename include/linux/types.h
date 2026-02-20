/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

#include <aerosync/types.h>

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};


/**
 * struct rcu_head - callback structure for call_rcu()
 */
struct callback_head {
  struct callback_head *next;
  void (*func)(struct callback_head *head);
} __attribute__((aligned(sizeof(void *))));
#define rcu_head callback_head

typedef void (*rcu_callback_t)(struct rcu_head *head);
typedef void (*call_rcu_func_t)(struct rcu_head *head, rcu_callback_t func);

typedef void (*swap_r_func_t)(void *a, void *b, int size, const void *priv);
typedef void (*swap_func_t)(void *a, void *b, int size);

typedef int (*cmp_r_func_t)(const void *a, const void *b, const void *priv);
typedef int (*cmp_func_t)(const void *a, const void *b);

/*
 * Below are truly Linux-specific types that should never collide with
 * any application/library that wants linux/types.h.
 */

/* sparse defines __CHECKER__; see Documentation/dev-tools/sparse.rst */
#define __bitwise	__attribute__((bitwise))

/* The kernel doesn't use this legacy form, but user space does */
#define __bitwise__ __bitwise

typedef uint16_t __bitwise __le16;
typedef uint16_t __bitwise __be16;
typedef uint32_t __bitwise __le32;
typedef uint32_t __bitwise __be32;
typedef uint64_t __bitwise __le64;
typedef uint64_t __bitwise __be64;

typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint64_t __u64;
typedef int64_t __s64;

typedef uint16_t __bitwise __sum16;
typedef uint32_t __bitwise __wsum;

/*
 * aligned_u64 should be used in defining kernel<->userspace ABIs to avoid
 * common 32/64-bit compat problems.
 * 64-bit values align to 4-byte boundaries on x86_32 (and possibly other
 * architectures) and to 8-byte boundaries on 64-bit architectures.  The new
 * aligned_64 type enforces 8-byte alignment so that structs containing
 * aligned_64 values have the same alignment on 32-bit and 64-bit architectures.
 * No conversions are necessary between 32-bit user-space and a 64-bit kernel.
 */
#define __aligned_u64 uint64_t __attribute__((aligned(8)))
#define __aligned_s64 int64_t __attribute__((aligned(8)))
#define __aligned_be64 __be64 __attribute__((aligned(8)))
#define __aligned_le64 __le64 __attribute__((aligned(8)))

typedef unsigned __bitwise __poll_t;

typedef long off_t;