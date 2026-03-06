/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file mm/vfs.c
 * @brief Memory Management VFS Integration
 * @copyright (C) 2026 assembler-0
 */

#include <mm/mm_types.h>
#include <mm/vm_tuning.h>
#include <fs/sysfs.h>
#include <aerosync/resdomain.h>
#include <lib/string.h>
#include <lib/uaccess.h>

/* --- sysfs: /runtime/sys/mm/ tuning --- */

static ssize_t mm_ksm_enabled_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[4];
  extern bool ksm_enabled;
  const int len = snprintf(kbuf, sizeof(kbuf), "%d", ksm_enabled);
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static const struct file_operations mm_ksm_fops = {
  .read = mm_ksm_enabled_read,
};

/* VM Tuning: eager_copy_threshold */
static ssize_t vm_eager_copy_threshold_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[16];
  int len = snprintf(kbuf, sizeof(kbuf), "%d\n", VM_EAGER_COPY_THRESHOLD());
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t vm_eager_copy_threshold_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file; (void) ppos;
  char kbuf[16];
  if (count >= sizeof(kbuf)) return -EINVAL;
  
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  
  int val = 0;
  if (sscanf(kbuf, "%d", &val) != 1) return -EINVAL;
  if (val < 2 || val > 16) return -EINVAL;  /* Range: 2-16 */
  
  atomic_set(&vm_tuning.eager_copy_threshold, val);
  return (ssize_t) count;
}

static const struct file_operations vm_eager_copy_threshold_fops = {
  .read = vm_eager_copy_threshold_read,
  .write = vm_eager_copy_threshold_write,
};

/* VM Tuning: tlb_batch_size */
static ssize_t vm_tlb_batch_size_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[16];
  int len = snprintf(kbuf, sizeof(kbuf), "%d\n", VM_TLB_BATCH_SIZE());
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t vm_tlb_batch_size_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file; (void) ppos;
  char kbuf[16];
  if (count >= sizeof(kbuf)) return -EINVAL;
  
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  
  int val = 0;
  if (sscanf(kbuf, "%d", &val) != 1) return -EINVAL;
  if (val < 8 || val > 256) return -EINVAL;  /* Range: 8-256 */
  
  atomic_set(&vm_tuning.tlb_batch_size, val);
  return (ssize_t) count;
}

static const struct file_operations vm_tlb_batch_size_fops = {
  .read = vm_tlb_batch_size_read,
  .write = vm_tlb_batch_size_write,
};

/* VM Tuning: collapse_threshold */
static ssize_t vm_collapse_threshold_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[16];
  int len = snprintf(kbuf, sizeof(kbuf), "%d\n", VM_COLLAPSE_THRESHOLD());
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t vm_collapse_threshold_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file; (void) ppos;
  char kbuf[16];
  if (count >= sizeof(kbuf)) return -EINVAL;
  
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  
  int val = 0;
  if (sscanf(kbuf, "%d", &val) != 1) return -EINVAL;
  if (val < 2 || val > 16) return -EINVAL;  /* Range: 2-16 */
  
  atomic_set(&vm_tuning.collapse_threshold, val);
  return (ssize_t) count;
}

static const struct file_operations vm_collapse_threshold_fops = {
  .read = vm_collapse_threshold_read,
  .write = vm_collapse_threshold_write,
};

/* VM Tuning: shadow_max_depth */
static ssize_t vm_shadow_max_depth_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file;
  char kbuf[16];
  int len = snprintf(kbuf, sizeof(kbuf), "%d\n", VM_SHADOW_MAX_DEPTH());
  return simple_read_from_buffer(buf, count, ppos, kbuf, (size_t) len);
}

static ssize_t vm_shadow_max_depth_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) file; (void) ppos;
  char kbuf[16];
  if (count >= sizeof(kbuf)) return -EINVAL;
  
  if (copy_from_user(kbuf, buf, count)) return -EFAULT;
  kbuf[count] = 0;
  
  int val = 0;
  if (sscanf(kbuf, "%d", &val) != 1) return -EINVAL;
  if (val < 4 || val > 32) return -EINVAL;  /* Range: 4-32 */
  
  atomic_set(&vm_tuning.shadow_max_depth, val);
  return (ssize_t) count;
}

static const struct file_operations vm_shadow_max_depth_fops = {
  .read = vm_shadow_max_depth_read,
  .write = vm_shadow_max_depth_write,
};

int mm_vfs_init(void) {
  /* sysfs tuning nodes */
  sysfs_create_dir_kern("ksm", "mm");
  sysfs_create_file_kern("enabled", &mm_ksm_fops, nullptr, "mm/ksm");

  /* VM tuning directory: /runtime/sys/mm/vm/ */
  sysfs_create_dir_kern("vm", "mm");
  sysfs_create_file_kern("eager_copy_threshold", &vm_eager_copy_threshold_fops, nullptr, "mm/vm");
  sysfs_create_file_kern("tlb_batch_size", &vm_tlb_batch_size_fops, nullptr, "mm/vm");
  sysfs_create_file_kern("collapse_threshold", &vm_collapse_threshold_fops, nullptr, "mm/vm");
  sysfs_create_file_kern("shadow_max_depth", &vm_shadow_max_depth_fops, nullptr, "mm/vm");

  sysfs_create_dir_kern("actl", "mm");
  return 0;
}
