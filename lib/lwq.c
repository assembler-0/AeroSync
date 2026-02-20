// SPDX-License-Identifier: GPL-2.0-only
/*
 * Light-weight single-linked queue.
 *
 * Entries are enqueued to the head of an llist, with no blocking.
 * This can happen in any context.
 *
 * Entries are dequeued using a spinlock to protect against multiple
 * access.  The llist is staged in reverse order, and refreshed
 * from the llist when it exhausts.
 *
 * This is particularly suitable when work items are queued in BH or
 * IRQ context, and where work items are handled one at a time by
 * dedicated threads.
 */
#include <linux/rcupdate.h>
#include <linux/lwq.h>
#include <aerosync/export.h>

struct llist_node *__lwq_dequeue(struct lwq *q)
{
	struct llist_node *this;

	if (lwq_empty(q))
		return nullptr;
	spin_lock(&q->lock);
	this = q->ready;
	if (!this && !llist_empty(&q->new)) {
		/* ensure queue doesn't appear transiently lwq_empty */
		smp_store_release(&q->ready, (void *)1);
		this = llist_reverse_order(llist_del_all(&q->new));
		if (!this)
			q->ready = nullptr;
	}
	if (this)
		q->ready = llist_next(this);
	spin_unlock(&q->lock);
	return this;
}
EXPORT_SYMBOL_GPL(__lwq_dequeue);

/**
 * lwq_dequeue_all - dequeue all currently enqueued objects
 * @q:	the queue to dequeue from
 *
 * Remove and return a linked list of llist_nodes of all the objects that were
 * in the queue. The first on the list will be the object that was least
 * recently enqueued.
 */
struct llist_node *lwq_dequeue_all(struct lwq *q)
{
	struct llist_node *r, *t, **ep;

	if (lwq_empty(q))
		return nullptr;

	spin_lock(&q->lock);
	r = q->ready;
	q->ready = nullptr;
	t = llist_del_all(&q->new);
	spin_unlock(&q->lock);
	ep = &r;
	while (*ep)
		ep = &(*ep)->next;
	*ep = llist_reverse_order(t);
	return r;
}
EXPORT_SYMBOL_GPL(lwq_dequeue_all);