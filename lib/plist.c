// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lib/plist.c
 *
 * Descending-priority-sorted double-linked list
 *
 * (C) 2002-2003 Intel Corp
 * Inaky Perez-Gonzalez <inaky.perez-gonzalez@intel.com>.
 *
 * 2001-2005 (c) MontaVista Software, Inc.
 * Daniel Walker <dwalker@mvista.com>
 *
 * (C) 2005 Linutronix GmbH, Thomas Gleixner <tglx@kernel.org>
 *
 * Simplifications of the original code by
 * Oleg Nesterov <oleg@tv-sign.ru>
 *
 * Based on simple lists (include/linux/list.h).
 *
 * This file contains the add / del functions which are considered to
 * be too large to inline. See include/linux/plist.h for further
 * information.
 */

#include <linux/plist.h>
#include <aerosync/errno.h>

#ifdef CONFIG_DEBUG_PLIST

static struct plist_head test_head;

static void plist_check_prev_next(struct list_head *t, struct list_head *p,
				  struct list_head *n)
{
	WARN_ON(n->prev != p || p->next != n);
}

static void plist_check_list(struct list_head *top)
{
	struct list_head *prev = top, *next = top->next;

	plist_check_prev_next(top, prev, next);
	while (next != top) {
		prev = next;
		next = prev->next;
		plist_check_prev_next(top, prev, next);
	}
}

static void plist_check_head(struct plist_head *head)
{
	if (!plist_head_empty(head))
		plist_check_list(&plist_first(head)->prio_list);
	plist_check_list(&head->node_list);
}

#else
# define plist_check_head(h)	do { } while (0)
#endif

/**
 * plist_add - add @node to @head
 *
 * @node:	&struct plist_node pointer
 * @head:	&struct plist_head pointer
 */
void plist_add(struct plist_node *node, struct plist_head *head)
{
	struct plist_node *first, *iter, *prev = nullptr, *last, *reverse_iter;
	struct list_head *node_next = &head->node_list;

	plist_check_head(head);
	WARN_ON(!plist_node_empty(node));
	WARN_ON(!list_empty(&node->prio_list));

	if (plist_head_empty(head))
		goto ins_node;

	first = iter = plist_first(head);
	last = reverse_iter = list_entry(first->prio_list.prev, struct plist_node, prio_list);

	do {
		if (node->prio < iter->prio) {
			node_next = &iter->node_list;
			break;
		} else if (node->prio >= reverse_iter->prio) {
			prev = reverse_iter;
			iter = list_entry(reverse_iter->prio_list.next,
				struct plist_node, prio_list);
			if (likely(reverse_iter != last))
				node_next = &iter->node_list;
			break;
		}

		prev = iter;
		iter = list_entry(iter->prio_list.next,
				struct plist_node, prio_list);
		reverse_iter = list_entry(reverse_iter->prio_list.prev,
				struct plist_node, prio_list);
	} while (iter != first);

	if (!prev || prev->prio != node->prio)
		list_add_tail(&node->prio_list, &iter->prio_list);
ins_node:
	list_add_tail(&node->node_list, node_next);

	plist_check_head(head);
}

/**
 * plist_del - Remove a @node from plist.
 *
 * @node:	&struct plist_node pointer - entry to be removed
 * @head:	&struct plist_head pointer - list head
 */
void plist_del(struct plist_node *node, struct plist_head *head)
{
	plist_check_head(head);

	if (!list_empty(&node->prio_list)) {
		if (node->node_list.next != &head->node_list) {
			struct plist_node *next;

			next = list_entry(node->node_list.next,
					struct plist_node, node_list);

			/* add the next plist_node into prio_list */
			if (list_empty(&next->prio_list))
				list_add(&next->prio_list, &node->prio_list);
		}
		list_del_init(&node->prio_list);
	}

	list_del_init(&node->node_list);

	plist_check_head(head);
}

/**
 * plist_requeue - Requeue @node at end of same-prio entries.
 *
 * This is essentially an optimized plist_del() followed by
 * plist_add().  It moves an entry already in the plist to
 * after any other same-priority entries.
 *
 * @node:	&struct plist_node pointer - entry to be moved
 * @head:	&struct plist_head pointer - list head
 */
void plist_requeue(struct plist_node *node, struct plist_head *head)
{
	struct plist_node *iter;
	struct list_head *node_next = &head->node_list;

	plist_check_head(head);
	BUG_ON(plist_head_empty(head));
	BUG_ON(plist_node_empty(node));

	if (node == plist_last(head))
		return;

	iter = plist_next(node);

	if (node->prio != iter->prio)
		return;

	plist_del(node, head);

	/*
	 * After plist_del(), iter is the replacement of the node.  If the node
	 * was on prio_list, take shortcut to find node_next instead of looping.
	 */
	if (!list_empty(&iter->prio_list)) {
		iter = list_entry(iter->prio_list.next, struct plist_node,
				  prio_list);
		node_next = &iter->node_list;
		goto queue;
	}

	plist_for_each_continue(iter, head) {
		if (node->prio != iter->prio) {
			node_next = &iter->node_list;
			break;
		}
	}
queue:
	list_add_tail(&node->node_list, node_next);

	plist_check_head(head);
}