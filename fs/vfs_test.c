/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/vfs_test.c
 * @brief VFS and MemFS Integrity Tests
 * @copyright (C) 2026 assembler-0
 */

#ifdef CONFIG_VFS_TESTS

#include <fs/vfs.h>
#include <lib/printk.h>
#include <fs/file.h>
#include <lib/string.h>
#include <aerosync/classes.h>
#include <aerosync/sysintf/device.h>
#include <aerosync/sysintf/attribute.h>
#include <aerosync/sysintf/char.h>

static int test_tmpfs_persistence(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing tmpfs persistence...");

  const char *path = "/tmp_test_file";
  const char *data = "AeroSync VFS Test Data";
  char buf[64];

  /* 1. Create and write */
  struct file *f = vfs_open(path, O_CREAT | O_RDWR, 0644);
  if (!f) {
    printkln(KERN_DEBUG TEST_CLASS "failed: vfs_open(O_CREAT) failed");
    return -1;
  }
  /* Set kernel mode for internal IO */
  f->f_mode |= FMODE_KERNEL;

  vfs_loff_t pos = 0;
  ssize_t written = vfs_write(f, data, strlen(data), &pos);
  if (written < 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: vfs_write failed, ret=%ld", written);
    vfs_close(f);
    return -1;
  }
  vfs_close(f);

  /* 2. Reopen and read */
  f = vfs_open(path, O_RDONLY, 0);
  if (!f) {
    printkln(KERN_DEBUG TEST_CLASS "failed: vfs_open(reopen) failed");
    return -1;
  }
  f->f_mode |= FMODE_KERNEL;

  pos = 0;
  memset(buf, 0, sizeof(buf));
  ssize_t read = vfs_read(f, buf, sizeof(buf), &pos);
  if (read < 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: vfs_read failed, ret=%ld", read);
    vfs_close(f);
    return -1;
  }

  if (strcmp(buf, data) != 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: data mismatch! read: '%s', expected: '%s'", buf, data);
    vfs_close(f);
    return -1;
  }

  vfs_close(f);
  printkln(KERN_DEBUG TEST_CLASS "passed: tmpfs persistence");
  return 0;
}

static int test_inode_identity(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing inode identity...");

  const char *path = "/tmp_identity_test";
  struct file *f1 = vfs_open(path, O_CREAT | O_RDWR, 0644);
  struct file *f2 = vfs_open(path, O_RDONLY, 0);

  if (!f1 || !f2) {
    printkln(KERN_DEBUG TEST_CLASS "failed: open failed");
    if (f1) vfs_close(f1);
    if (f2) vfs_close(f2);
    return -1;
  }

  if (f1->f_inode != f2->f_inode) {
    printkln(KERN_DEBUG TEST_CLASS "failed: Inode mismatch! f1->inode=%p, f2->inode=%p", f1->f_inode, f2->f_inode);
    vfs_close(f1);
    vfs_close(f2);
    return -1;
  }

  vfs_close(f1);
  vfs_close(f2);
  printkln(KERN_DEBUG TEST_CLASS "passed: Inode identity");
  return 0;
}

/* --- Smoke Tests for AeroSync HFS Integration --- */

static int test_devtmpfs_nodes(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing devtmpfs nodes...");

  struct dentry *d = vfs_path_lookup("/runtime/devices/misc/crypto", 0);
  if (!d) {
    printkln(KERN_DEBUG TEST_CLASS "failed: /runtime/devices/misc/crypto not found");
    return -1;
  }

  if (!S_ISCHR(d->d_inode->i_mode)) {
    printkln(KERN_DEBUG TEST_CLASS "failed: crypto node is not a character device");
    dput(d);
    return -1;
  }

  if (d->d_inode->i_rdev != MKDEV(10, 235)) {
    printkln(KERN_DEBUG TEST_CLASS "failed: crypto node has wrong device number: %llx", d->d_inode->i_rdev);
    dput(d);
    return -1;
  }

  dput(d);
  printkln(KERN_DEBUG TEST_CLASS "passed: devtmpfs nodes");
  return 0;
}

static int test_sysfs_structure(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing sysfs structure...");

  const char *paths[] = {
    "/runtime/sys/sched",
    "/runtime/sys/mm",
    "/runtime/sys/actl/sched",
    "/runtime/sys/actl/mm",
    "/runtime/sys/devices"
  };

  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    struct dentry *d = vfs_path_lookup(paths[i], 0);
    if (!d) {
      printkln(KERN_DEBUG TEST_CLASS "failed: sysfs path %s not found", paths[i]);
      return -1;
    }
    if (!S_ISDIR(d->d_inode->i_mode)) {
      printkln(KERN_DEBUG TEST_CLASS "failed: %s is not a directory", paths[i]);
      dput(d);
      return -1;
    }
    dput(d);
  }

  printkln(KERN_DEBUG TEST_CLASS "passed: sysfs structure");
  return 0;
}

static ssize_t mock_show(struct device *dev, struct device_attribute *attr, char *buf) {
  (void) dev;
  (void) attr;
  return snprintf(buf, 32, "mock_value\n");
}

static int mock_val = 0;

static ssize_t mock_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count) {
  (void) dev;
  (void) attr;
  mock_val = (int) count;
  return count;
}

static DEVICE_ATTR(test_attr, 0644, mock_show, mock_store);
static struct attribute *mock_attrs[] = {&dev_attr_test_attr.attr, nullptr};
static struct attribute_group mock_group = {.attrs = mock_attrs};
static const struct attribute_group *mock_groups[] = {&mock_group, nullptr};

static void mock_release(struct device *dev) {
  (void) dev;
}

static struct device mock_dev = {
  .name = "vfs_mock_dev",
  .groups = mock_groups,
  .release = mock_release,
};

static int test_sysfs_attributes(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing sysfs attributes...");

  device_initialize(&mock_dev);
  int ret = device_add(&mock_dev);
  if (ret < 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: device_add failed: %d", ret);
    return -1;
  }

  /* Verify path exists */
  const char *attr_path = "/runtime/sys/devices/vfs_mock_dev/test_attr";
  struct file *f = vfs_open(attr_path, O_RDWR, 0);
  if (!f) {
    printkln(KERN_DEBUG TEST_CLASS "failed: attribute file %s not found", attr_path);
    device_unregister(&mock_dev);
    return -1;
  }
  f->f_mode |= FMODE_KERNEL;

  /* Test read */
  char buf[32];
  vfs_loff_t pos = 0;
  ssize_t r = vfs_read(f, buf, sizeof(buf) - 1, &pos);
  if (r < 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: attribute read failed: %d", (int) r);
    vfs_close(f);
    device_unregister(&mock_dev);
    return -1;
  }
  buf[r] = '\0';

  if (strcmp(buf, "mock_value\n") != 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: attribute read mismatch: '%s'", buf);
    vfs_close(f);
    device_unregister(&mock_dev);
    return -1;
  }

  /* Test write */
  pos = 0;
  mock_val = 0;
  const char *test_data = "hello";
  vfs_write(f, test_data, strlen(test_data), &pos);
  if (mock_val != (int) strlen(test_data)) {
    printkln(KERN_DEBUG TEST_CLASS "failed: attribute store callback not triggered correctly");
    vfs_close(f);
    device_unregister(&mock_dev);
    return -1;
  }

  vfs_close(f);
  device_unregister(&mock_dev);

  /* Verify unregister removed the node */
  struct dentry *d = vfs_path_lookup("/runtime/sys/devices/vfs_mock_dev", 0);
  if (d) {
    printkln(KERN_DEBUG TEST_CLASS "failed: sysfs device directory still exists after unregister");
    dput(d);
    return -1;
  }

  printkln(KERN_DEBUG TEST_CLASS "passed: sysfs attributes");
  return 0;
}

static int test_procfs_stats(void) {
  printkln(KERN_DEBUG TEST_CLASS "testing procfs stats...");

  struct file *f = vfs_open("/runtime/processes/sched_stats", O_RDONLY, 0);
  if (!f) {
    printkln(KERN_DEBUG TEST_CLASS "failed: /runtime/processes/sched_stats not found");
    return -1;
  }
  f->f_mode |= FMODE_KERNEL;

  char buf[128];
  vfs_loff_t pos = 0;
  ssize_t r = vfs_read(f, buf, sizeof(buf) - 1, &pos);
  if (r <= 0) {
    printkln(KERN_DEBUG TEST_CLASS "failed: failed to read sched_stats");
    vfs_close(f);
    return -1;
  }
  buf[r] = '\0';

  if (strstr(buf, "Scheduler Statistics") == nullptr) {
    printkln(KERN_DEBUG TEST_CLASS "failed: sched_stats content invalid: '%s'", buf);
    vfs_close(f);
    return -1;
  }

  vfs_close(f);
  printkln(KERN_DEBUG TEST_CLASS "passed: procfs stats");
  return 0;
}

void vfs_run_tests(void) {
  printkln(KERN_DEBUG TEST_CLASS "starting vfs smoke test");

  int failed = 0;
  if (test_tmpfs_persistence() < 0) failed++;
  if (test_inode_identity() < 0) failed++;
  if (test_devtmpfs_nodes() < 0) failed++;
  if (test_sysfs_structure() < 0) failed++;
  if (test_sysfs_attributes() < 0) failed++;
  if (test_procfs_stats() < 0) failed++;

  if (failed) {
    printkln(KERN_DEBUG TEST_CLASS "some vfs test failed: %d failures", failed);
  } else {
    printkln(KERN_DEBUG TEST_CLASS "all vfs tests passed.");
  }
}

#endif