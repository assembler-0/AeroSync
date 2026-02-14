#pragma once

#include <aerosync/types.h>
#include <linux/list.h>
#include <aerosync/spinlock.h>

/* VFS Event Types */
#define VFS_EVENT_ACCESS      0x00000001
#define VFS_EVENT_MODIFY      0x00000002
#define VFS_EVENT_ATTRIB      0x00000004
#define VFS_EVENT_CLOSE_WRITE 0x00000008
#define VFS_EVENT_CLOSE_READ  0x00000010
#define VFS_EVENT_OPEN        0x00000020
#define VFS_EVENT_MOVED_FROM  0x00000040
#define VFS_EVENT_MOVED_TO    0x00000080
#define VFS_EVENT_CREATE      0x00000100
#define VFS_EVENT_DELETE      0x00000200
#define VFS_EVENT_DELETE_SELF 0x00000400
#define VFS_EVENT_MOVE_SELF   0x00000800

struct vfs_event_subscriber;

typedef void (*vfs_event_callback_t)(struct vfs_event_subscriber *sub, uint32_t mask, struct dentry *dentry);

struct vfs_event_subscriber {
    struct list_head list;
    uint32_t mask;
    vfs_event_callback_t callback;
    void *priv;
};

void vfs_event_init(void);
int vfs_event_subscribe(struct dentry *dentry, uint32_t mask, vfs_event_callback_t callback, void *priv);
void vfs_event_unsubscribe(struct dentry *dentry, struct vfs_event_subscriber *sub);
void vfs_notify_change(struct dentry *dentry, uint32_t event);
