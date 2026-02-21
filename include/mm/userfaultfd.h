#pragma once

#include <aerosync/atomic.h>
#include <aerosync/spinlock.h>
#include <aerosync/types.h>
#include <aerosync/wait.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <linux/list.h>
#include <mm/mm_types.h>

/* Standard IOCTL macros */
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2

#define _IOC_NRMASK ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U

#define _IOC(dir, type, nr, size)                                              \
  (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) |                     \
   ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))

#define _IOWR(type, nr, size)                                                  \
  _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (sizeof(size)))
#define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), (sizeof(size)))
#define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), (sizeof(size)))

#define _IOC_NR(nr) (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)

/* userfaultfd msg types */
#define UFFD_EVENT_PAGEFAULT 0x11
#define UFFD_EVENT_FORK 0x12
#define UFFD_EVENT_REMAP 0x13
#define UFFD_EVENT_REMOVE 0x14
#define UFFD_EVENT_UNMAP 0x15

struct uffd_msg {
  uint8_t event;
  uint8_t reserved1;
  uint16_t reserved2;
  uint32_t reserved3;
  union {
    struct {
      uint64_t flags;
      uint64_t address;
      union {
        struct {
          uint64_t ptid;
        } pagefault;
      } feat;
    } pagefault;
    struct {
      uint32_t ufd;
    } fork;
    struct {
      uint64_t from;
      uint64_t to;
      uint64_t len;
    } remap;
    struct {
      uint64_t start;
      uint64_t end;
    } remove;
    struct {
      uint64_t start;
      uint64_t end;
    } unmap;
  } arg;
};

/* uffd msg flags */
#define UFFD_PAGEFAULT_FLAG_WRITE (1 << 0)
#define UFFD_PAGEFAULT_FLAG_WP (1 << 1)

struct userfaultfd_ctx {
  atomic_t refcount;
  struct mm_struct *mm;
  wait_queue_head_t fault_wqh;
  wait_queue_head_t fd_wqh;
  struct list_head event_list;
  spinlock_t event_list_lock;
  unsigned int flags;
};

struct uffd_wait_queue {
  struct uffd_msg msg;
  wait_queue_entry_t wait;
  struct userfaultfd_ctx *ctx;
  bool awakened;
};

struct uffd_event {
  struct uffd_msg msg;
  struct list_head list;
};

/* ioctls */
#define UFFDIO_API _IOWR('u', 0x01, struct uffdio_api)
#define UFFDIO_REGISTER _IOWR('u', 0x02, struct uffdio_register)
#define UFFDIO_UNREGISTER _IOW('u', 0x03, struct uffdio_range)
#define UFFDIO_WAKE _IOW('u', 0x04, struct uffdio_range)
#define UFFDIO_COPY _IOWR('u', 0x05, struct uffdio_copy)
#define UFFDIO_ZEROPAGE _IOWR('u', 0x06, struct uffdio_zeropage)
#define UFFDIO_WRITEPROTECT _IOWR('u', 0x07, struct uffdio_writeprotect)
#define UFFDIO_CONTINUE _IOWR('u', 0x08, struct uffdio_continue)

struct uffdio_api {
  uint64_t api;
  uint64_t features;
  uint64_t ioctls;
};

struct uffdio_range {
  uint64_t start;
  uint64_t len;
};

struct uffdio_register {
  struct uffdio_range range;
  uint64_t mode;
  uint64_t ioctls;
};

struct uffdio_copy {
  uint64_t dst;
  uint64_t src;
  uint64_t len;
  uint64_t mode;
  int64_t copy;
};

struct uffdio_zeropage {
  struct uffdio_range range;
  uint64_t mode;
  int64_t zeropage;
};

struct uffdio_writeprotect {
  struct uffdio_range range;
  uint64_t mode;
};

struct uffdio_continue {
  struct uffdio_range range;
  uint64_t mode;
  int64_t mapped;
};

#define UFFDIO_REGISTER_MODE_MISSING ((uint64_t)1 << 0)
#define UFFDIO_REGISTER_MODE_WP ((uint64_t)1 << 1)
#define UFFDIO_REGISTER_MODE_MINOR ((uint64_t)1 << 2)

#define UFFD_API 0xAA

void userfaultfd_ctx_get(struct userfaultfd_ctx *ctx);
void userfaultfd_ctx_put(struct userfaultfd_ctx *ctx);

struct syscall_regs;
void sys_userfaultfd(struct syscall_regs *regs);

int handle_userfault(struct vm_area_struct *vma, uint64_t address,
                     unsigned int flags, uint32_t reason);
