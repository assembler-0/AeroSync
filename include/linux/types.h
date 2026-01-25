/* SPDX-License-Identifier: GPL-2.0 */
#pragma once

struct list_head {
	struct list_head *next, *prev;
};

struct hlist_head {
	struct hlist_node *first;
};

struct hlist_node {
	struct hlist_node *next, **pprev;
};

typedef long off_t;