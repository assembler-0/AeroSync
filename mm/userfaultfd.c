#include <aerosync/atomic.h>
#include <aerosync/classes.h>
#include <aerosync/errno.h>
#include <aerosync/export.h>
#include <aerosync/panic.h>
#include <aerosync/sched/process.h>
#include <aerosync/spinlock.h>
#include <arch/x86_64/entry.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <lib/printk.h>
#include <lib/string.h>
#include <lib/uaccess.h>
#include <linux/list.h>
#include <mm/slub.h>
#include <mm/userfaultfd.h>
#include <mm/vma.h>

void userfaultfd_ctx_get(struct userfaultfd_ctx *ctx) {
  atomic_inc(&ctx->refcount);
}

void userfaultfd_ctx_put(struct userfaultfd_ctx *ctx) {
  if (atomic_dec_and_test(&ctx->refcount)) {
    struct uffd_event *event, *tmp;
    spinlock_lock(&ctx->event_list_lock);
    list_for_each_entry_safe(event, tmp, &ctx->event_list, list) {
      list_del(&event->list);
      kfree(event);
    }
    spinlock_unlock(&ctx->event_list_lock);
    if (ctx->mm) {
      mm_put(ctx->mm);
    }
    kfree(ctx);
  }
}

static int uffd_release(struct inode *inode, struct file *file) {
  (void)inode;
  struct userfaultfd_ctx *ctx = file->private_data;
  if (ctx) {
    userfaultfd_ctx_put(ctx);
  }
  return 0;
}

static ssize_t uffd_read(struct file *file, char *buf, size_t count,
                         vfs_loff_t *ppos) {
  (void)ppos;
  struct userfaultfd_ctx *ctx = file->private_data;
  ssize_t total_read = 0;

  if (count < sizeof(struct uffd_msg))
    return -EINVAL;

  while (total_read + sizeof(struct uffd_msg) <= count) {
    struct uffd_event *event = nullptr;

    spinlock_lock(&ctx->event_list_lock);
    if (!list_empty(&ctx->event_list)) {
      event = list_first_entry(&ctx->event_list, struct uffd_event, list);
      list_del(&event->list);
    }
    spinlock_unlock(&ctx->event_list_lock);

    if (!event) {
      if (total_read > 0)
        break;
      if (file->f_flags & O_NONBLOCK)
        return -EAGAIN;

      wait_event_interruptible(ctx->fd_wqh, !list_empty(&ctx->event_list));
      continue;
    }

    if (copy_to_user(buf + total_read, &event->msg, sizeof(struct uffd_msg))) {
      spinlock_lock(&ctx->event_list_lock);
      list_add(&event->list, &ctx->event_list);
      spinlock_unlock(&ctx->event_list_lock);
      return -EFAULT;
    }

    kfree(event);
    total_read += sizeof(struct uffd_msg);
  }

  return total_read;
}

static int uffd_register(struct userfaultfd_ctx *ctx, unsigned long arg) {
  struct uffdio_register reg;
  if (copy_from_user(&reg, (void *)arg, sizeof(reg)))
    return -EFAULT;

  uint64_t start = reg.range.start;
  uint64_t len = reg.range.len;
  uint64_t end = start + len;

  if (start & (PAGE_SIZE - 1) || len & (PAGE_SIZE - 1))
    return -EINVAL;
  if (end <= start)
    return -EINVAL;

  struct mm_struct *mm = ctx->mm;
  struct vm_area_struct *vma;

  down_write(&mm->mmap_lock);
  for_each_vma_range(mm, vma, start, end) {
    if (vma->vm_userfaultfd_ctx != ctx) {
      if (vma->vm_userfaultfd_ctx) {
        userfaultfd_ctx_put(vma->vm_userfaultfd_ctx);
      }
      userfaultfd_ctx_get(ctx);
      vma->vm_userfaultfd_ctx = ctx;
    }

    if (reg.mode & UFFDIO_REGISTER_MODE_MISSING) {
      vma->vm_flags |= VM_UFFD_MISSING;
    }
    if (reg.mode & UFFDIO_REGISTER_MODE_WP) {
      vma->vm_flags |= VM_UFFD_WP;
    }
  }
  up_write(&mm->mmap_lock);

  reg.ioctls =
      (1ULL << _IOC_NR(UFFDIO_COPY)) | (1ULL << _IOC_NR(UFFDIO_ZEROPAGE));
  if (copy_to_user((void *)arg, &reg, sizeof(reg)))
    return -EFAULT;

  return 0;
}

static int uffd_unregister(struct userfaultfd_ctx *ctx, unsigned long arg) {
  struct uffdio_range range;
  if (copy_from_user(&range, (void *)arg, sizeof(range)))
    return -EFAULT;

  uint64_t start = range.start;
  uint64_t len = range.len;
  uint64_t end = start + len;

  if (start & (PAGE_SIZE - 1) || len & (PAGE_SIZE - 1))
    return -EINVAL;
  if (end <= start)
    return -EINVAL;

  struct mm_struct *mm = ctx->mm;
  struct vm_area_struct *vma;

  down_write(&mm->mmap_lock);
  for_each_vma_range(mm, vma, start, end) {
    if (vma->vm_userfaultfd_ctx == ctx) {
      userfaultfd_ctx_put(vma->vm_userfaultfd_ctx);
      vma->vm_userfaultfd_ctx = nullptr;
      vma->vm_flags &= ~(VM_UFFD_MISSING | VM_UFFD_WP);
    }
  }
  up_write(&mm->mmap_lock);

  return 0;
}

static int uffd_copy(struct userfaultfd_ctx *ctx, unsigned long arg) {
  struct uffdio_copy copy;
  if (copy_from_user(&copy, (void *)arg, sizeof(copy)))
    return -EFAULT;

  uint64_t dst = copy.dst;
  uint64_t src = copy.src;
  uint64_t len = copy.len;

  if (dst & (PAGE_SIZE - 1) || src & (PAGE_SIZE - 1) || len & (PAGE_SIZE - 1))
    return -EINVAL;
  if (dst + len <= dst)
    return -EINVAL;

  uint64_t total_copied = 0;
  while (total_copied < len) {
    uint64_t phys = pmm_alloc_page();
    if (!phys)
      return -ENOMEM;

    void *kaddr = pmm_phys_to_virt(phys);
    if (copy_from_user(kaddr, (void *)(src + total_copied), PAGE_SIZE)) {
      pmm_free_page(phys);
      return -EFAULT;
    }

    down_write(&ctx->mm->mmap_lock);
    if (vmm_virt_to_phys(ctx->mm, dst + total_copied) != 0) {
      up_write(&ctx->mm->mmap_lock);
      pmm_free_page(phys);
      return -EEXIST;
    }
    int ret = vmm_map_page(ctx->mm, dst + total_copied, phys,
                           PTE_PRESENT | PTE_RW | PTE_USER);
    up_write(&ctx->mm->mmap_lock);

    if (ret < 0) {
      pmm_free_page(phys);
      return -EFAULT;
    }

    total_copied += PAGE_SIZE;
  }

  wake_up_all(&ctx->fault_wqh);

  copy.copy = total_copied;
  if (copy_to_user((void *)arg, &copy, sizeof(copy)))
    return -EFAULT;

  return 0;
}

static int uffd_zeropage(struct userfaultfd_ctx *ctx, unsigned long arg) {
  struct uffdio_zeropage zp;
  if (copy_from_user(&zp, (void *)arg, sizeof(zp)))
    return -EFAULT;

  uint64_t start = zp.range.start;
  uint64_t len = zp.range.len;

  if (start & (PAGE_SIZE - 1) || len & (PAGE_SIZE - 1))
    return -EINVAL;

  uint64_t total_zeroed = 0;
  while (total_zeroed < len) {
    uint64_t phys = pmm_alloc_page();
    if (!phys)
      return -ENOMEM;

    void *kaddr = pmm_phys_to_virt(phys);
    memset(kaddr, 0, PAGE_SIZE);

    down_write(&ctx->mm->mmap_lock);
    if (vmm_virt_to_phys(ctx->mm, start + total_zeroed) != 0) {
      up_write(&ctx->mm->mmap_lock);
      pmm_free_page(phys);
      return -EEXIST;
    }
    int ret = vmm_map_page(ctx->mm, start + total_zeroed, phys,
                           PTE_PRESENT | PTE_RW | PTE_USER);
    up_write(&ctx->mm->mmap_lock);

    if (ret < 0) {
      pmm_free_page(phys);
      return -EFAULT;
    }

    total_zeroed += PAGE_SIZE;
  }

  wake_up_all(&ctx->fault_wqh);

  zp.zeropage = total_zeroed;
  if (copy_to_user((void *)arg, &zp, sizeof(zp)))
    return -EFAULT;

  return 0;
}

static int uffd_wake(struct userfaultfd_ctx *ctx, unsigned long arg) {
  struct uffdio_range range;
  if (copy_from_user(&range, (void *)arg, sizeof(range)))
    return -EFAULT;

  wake_up_all(&ctx->fault_wqh);
  return 0;
}

static int uffd_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
  struct userfaultfd_ctx *ctx = file->private_data;

  switch (cmd) {
  case UFFDIO_API: {
    struct uffdio_api api;
    if (copy_from_user(&api, (void *)arg, sizeof(api)))
      return -EFAULT;
    if (api.api != UFFD_API)
      return -EINVAL;
    api.features = 0;
    api.ioctls =
        (1ULL << _IOC_NR(UFFDIO_REGISTER)) |
        (1ULL << _IOC_NR(UFFDIO_UNREGISTER)) | (1ULL << _IOC_NR(UFFDIO_COPY)) |
        (1ULL << _IOC_NR(UFFDIO_ZEROPAGE)) | (1ULL << _IOC_NR(UFFDIO_WAKE));
    if (copy_to_user((void *)arg, &api, sizeof(api)))
      return -EFAULT;
    return 0;
  }
  case UFFDIO_REGISTER:
    return uffd_register(ctx, arg);
  case UFFDIO_UNREGISTER:
    return uffd_unregister(ctx, arg);
  case UFFDIO_COPY:
    return uffd_copy(ctx, arg);
  case UFFDIO_ZEROPAGE:
    return uffd_zeropage(ctx, arg);
  case UFFDIO_WAKE:
    return uffd_wake(ctx, arg);
  default:
    return -ENOTTY;
  }
}

static const struct file_operations uffd_fops = {
    .read = uffd_read,
    .release = uffd_release,
    .ioctl = uffd_ioctl,
};

int handle_userfault(struct vm_area_struct *vma, uint64_t address,
                     unsigned int flags, uint32_t reason) {
  (void)reason;
  struct userfaultfd_ctx *ctx = vma->vm_userfaultfd_ctx;

  if (!ctx)
    return VM_FAULT_SIGBUS;

  struct uffd_msg msg = {
      .event = UFFD_EVENT_PAGEFAULT,
      .arg.pagefault.address = address & PAGE_MASK,
      .arg.pagefault.flags =
          (flags & FAULT_FLAG_WRITE) ? UFFD_PAGEFAULT_FLAG_WRITE : 0,
  };

  struct uffd_event *event = kmalloc(sizeof(struct uffd_event));
  if (!event)
    return VM_FAULT_OOM;

  event->msg = msg;

  spinlock_lock(&ctx->event_list_lock);
  list_add_tail(&event->list, &ctx->event_list);
  spinlock_unlock(&ctx->event_list_lock);

  wake_up_interruptible(&ctx->fd_wqh);

  printk(KERN_INFO UFFD_CLASS "Thread %d sleeping for userfault at %llx\n",
         current->pid, address);

  /*
   * Drop locks before sleeping.
   * If this is a speculative fault, we don't hold mmap_lock yet,
   * so we return RETRY to fall back to the slow path.
   */
  if (flags & FAULT_FLAG_SPECULATIVE)
    return VM_FAULT_RETRY;

  up_read(&vma->vm_mm->mmap_lock);

  wait_event_interruptible(
      ctx->fault_wqh, (vmm_virt_to_phys(ctx->mm, address & PAGE_MASK) != 0));

  down_read(&vma->vm_mm->mmap_lock);

  return VM_FAULT_RETRY;
}

#define REGS_RETURN_VAL(r, v)                                                  \
  (r ? r->rax = v : panic(SYSCALL_CLASS "regs == null"))

void sys_userfaultfd(struct syscall_regs *regs) {
  int flags = (int)regs->rdi;
  struct userfaultfd_ctx *ctx = kmalloc(sizeof(struct userfaultfd_ctx));
  if (!ctx) {
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  atomic_set(&ctx->refcount, 1);
  ctx->mm = current->mm;
  mm_get(ctx->mm);
  init_waitqueue_head(&ctx->fault_wqh);
  init_waitqueue_head(&ctx->fd_wqh);
  INIT_LIST_HEAD(&ctx->event_list);
  spinlock_init(&ctx->event_list_lock);
  ctx->flags = flags;

  struct file *file = kmalloc(sizeof(struct file));
  if (!file) {
    userfaultfd_ctx_put(ctx);
    REGS_RETURN_VAL(regs, -ENOMEM);
    return;
  }

  memset(file, 0, sizeof(struct file));
  file->f_op = &uffd_fops;
  file->private_data = ctx;
  file->f_flags = flags & (O_CLOEXEC | O_NONBLOCK);
  atomic_set(&file->f_count, 1);

  int fd = get_unused_fd_flags(file->f_flags);
  if (fd < 0) {
    kfree(file);
    userfaultfd_ctx_put(ctx);
    REGS_RETURN_VAL(regs, fd);
    return;
  }

  fd_install(fd, file);
  REGS_RETURN_VAL(regs, fd);
}
