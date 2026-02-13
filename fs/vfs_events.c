/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/vfs_events.c
 * @brief VFS Event Notification System
 * @copyright (C) 2026 assembler-0
 */

#include <fs/vfs.h>
#include <fs/vfs_events.h>
#include <mm/slub.h>
#include <lib/printk.h>
#include <aerosync/errno.h>

void vfs_event_init(void) {
    /* Potential global init */
}

int vfs_event_subscribe(struct dentry *dentry, uint32_t mask, vfs_event_callback_t callback, void *priv) {
    if (!dentry || !callback) return -EINVAL;

    struct vfs_event_subscriber *sub = kmalloc(sizeof(*sub));
    if (!sub) return -ENOMEM;

    sub->mask = mask;
    sub->callback = callback;
    sub->priv = priv;

    spinlock_lock(&dentry->d_lock);
    if (list_empty(&dentry->d_subscribers)) {
        INIT_LIST_HEAD(&dentry->d_subscribers);
    }
    list_add(&sub->list, &dentry->d_subscribers);
    spinlock_unlock(&dentry->d_lock);

    return 0;
}

void vfs_event_unsubscribe(struct dentry *dentry, struct vfs_event_subscriber *sub) {
    if (!dentry || !sub) return;

    spinlock_lock(&dentry->d_lock);
    list_del(&sub->list);
    spinlock_unlock(&dentry->d_lock);
    kfree(sub);
}

void vfs_notify_change(struct dentry *dentry, uint32_t event) {
    if (!dentry) return;

    struct dentry *curr = dentry;
    while (curr) {
        spinlock_lock(&curr->d_lock);
        if (!list_empty(&curr->d_subscribers)) {
            struct vfs_event_subscriber *sub;
            list_for_each_entry(sub, &curr->d_subscribers, list) {
                if (sub->mask & event) {
                    /* Call callback (caution: d_lock is held) */
                    sub->callback(sub, event, dentry);
                }
            }
        }
        spinlock_unlock(&curr->d_lock);

        /* Traverse up for subtree notifications */
        struct dentry *parent = curr->d_parent;
        if (!parent || parent == curr) break;
        curr = parent;
    }
}
