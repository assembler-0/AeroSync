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


/**
 * struct rcu_head - callback structure for call_rcu()
 */
struct rcu_head {
  struct rcu_head *next;
  void (*func)(struct rcu_head *head);
};


typedef long off_t;