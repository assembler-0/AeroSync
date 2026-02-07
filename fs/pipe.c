/// SPDX-License-Identifier: GPL-2.0-only
/**
 * AeroSync monolithic kernel
 *
 * @file fs/pipe.c
 * @brief Anonymous Pipe implementation
 * @copyright (C) 2026 assembler-0
 *
 * This file is part of the AeroSync kernel.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <fs/vfs.h>
#include <fs/file.h>
#include <mm/slub.h>
#include <aerosync/mutex.h>
#include <aerosync/wait.h>
#include <aerosync/errno.h>
#include <lib/uaccess.h>

#define PIPE_BUF_SIZE 65536

struct pipe_inode_info {
  mutex_t lock;
  wait_queue_head_t rd_wait;
  wait_queue_head_t wr_wait;
  char *buffer;
  uint32_t head;
  uint32_t tail;
  uint32_t readers;
  uint32_t writers;
};

static void free_pipe_info(struct pipe_inode_info *pipe) {
  if (!pipe) return;
  if (pipe->buffer) kfree(pipe->buffer);
  kfree(pipe);
}

static ssize_t pipe_read(struct file *file, char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct pipe_inode_info *pipe = file->private_data;
  ssize_t ret = 0;

  mutex_lock(&pipe->lock);
  while (pipe->head == pipe->tail) {
    if (pipe->writers == 0) {
      mutex_unlock(&pipe->lock);
      return 0;
    }
    if (file->f_flags & O_NONBLOCK) {
      mutex_unlock(&pipe->lock);
      return -EAGAIN;
    }
    mutex_unlock(&pipe->lock);
    wait_event_interruptible(pipe->rd_wait, pipe->head != pipe->tail || pipe->writers == 0);
    mutex_lock(&pipe->lock);
  }

  size_t available = (pipe->head - pipe->tail) % PIPE_BUF_SIZE;
  if (count > available) count = available;

  for (size_t i = 0; i < count; i++) {
    char c = pipe->buffer[pipe->tail];
    if (file->f_mode & FMODE_KERNEL) {
      buf[i] = c;
    } else if (copy_to_user(buf + i, &c, 1) != 0) {
      if (ret == 0) ret = -EFAULT;
      break;
    }
    pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
    ret++;
  }

  if (ret > 0)
    wake_up(&pipe->wr_wait);
  mutex_unlock(&pipe->lock);
  return ret;
}

static ssize_t pipe_write(struct file *file, const char *buf, size_t count, vfs_loff_t *ppos) {
  (void) ppos;
  struct pipe_inode_info *pipe = file->private_data;
  ssize_t ret = 0;

  mutex_lock(&pipe->lock);
  if (pipe->readers == 0) {
    mutex_unlock(&pipe->lock);
    return -EPIPE;
  }

  while (((pipe->head + 1) % PIPE_BUF_SIZE) == pipe->tail) {
    if (file->f_flags & O_NONBLOCK) {
      mutex_unlock(&pipe->lock);
      return -EAGAIN;
    }
    mutex_unlock(&pipe->lock);
    wait_event_interruptible(pipe->wr_wait, ((pipe->head + 1) % PIPE_BUF_SIZE) != pipe->tail || pipe->readers == 0);
    mutex_lock(&pipe->lock);
    if (pipe->readers == 0) {
      mutex_unlock(&pipe->lock);
      return -EPIPE;
    }
  }

  size_t free_space = (pipe->tail - pipe->head - 1 + PIPE_BUF_SIZE) % PIPE_BUF_SIZE;
  if (count > free_space) count = free_space;

  for (size_t i = 0; i < count; i++) {
    char c;
    if (file->f_mode & FMODE_KERNEL) {
      c = buf[i];
    } else if (copy_from_user(&c, buf + i, 1) != 0) {
      if (ret == 0) ret = -EFAULT;
      break;
    }
    pipe->buffer[pipe->head] = c;
    pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
    ret++;
  }

  if (ret > 0)
    wake_up(&pipe->rd_wait);
  mutex_unlock(&pipe->lock);
  return ret;
}

static uint32_t pipe_poll(struct file *file, poll_table *pt) {
  struct pipe_inode_info *pipe = file->private_data;
  uint32_t mask = 0;

  mutex_lock(&pipe->lock);
  if (pipe->head != pipe->tail) mask |= POLLIN | POLLPRI;
  if (((pipe->head + 1) % PIPE_BUF_SIZE) != pipe->tail) mask |= POLLOUT;
  if (pipe->writers == 0) mask |= POLLHUP;
  if (pipe->readers == 0) mask |= POLLERR;
  mutex_unlock(&pipe->lock);

  return mask;
}

static int pipe_release(struct inode *inode, struct file *file) {
  (void) inode;
  struct pipe_inode_info *pipe = file->private_data;

  mutex_lock(&pipe->lock);
  if (file->f_mode & FMODE_READ) pipe->readers--;
  if (file->f_mode & FMODE_WRITE) pipe->writers--;

  if (pipe->readers == 0 && pipe->writers == 0) {
    mutex_unlock(&pipe->lock);
    free_pipe_info(pipe);
  } else {
    wake_up(&pipe->rd_wait);
    wake_up(&pipe->wr_wait);
    mutex_unlock(&pipe->lock);
  }
  return 0;
}

static struct file_operations pipe_rd_fops = {
  .read = pipe_read,
  .poll = pipe_poll,
  .release = pipe_release,
};

static struct file_operations pipe_wr_fops = {
  .write = pipe_write,
  .poll = pipe_poll,
  .release = pipe_release,
};

int do_pipe(int pipefd[2]) {
  struct pipe_inode_info *pipe = kzalloc(sizeof(*pipe));
  if (!pipe) return -ENOMEM;

  pipe->buffer = kmalloc(PIPE_BUF_SIZE);
  if (!pipe->buffer) {
    kfree(pipe);
    return -ENOMEM;
  }

  mutex_init(&pipe->lock);
  init_waitqueue_head(&pipe->rd_wait);
  init_waitqueue_head(&pipe->wr_wait);
  pipe->readers = pipe->writers = 1;

  struct file *f_rd = kzalloc(sizeof(struct file));
  struct file *f_wr = kzalloc(sizeof(struct file));

  if (!f_rd || !f_wr) {
    if (f_rd) kfree(f_rd);
    if (f_wr) kfree(f_wr);
    free_pipe_info(pipe);
    return -ENOMEM;
  }

  atomic_set(&f_rd->f_count, 1);
  f_rd->f_op = &pipe_rd_fops;
  f_rd->private_data = pipe;
  f_rd->f_mode = FMODE_READ;

  atomic_set(&f_wr->f_count, 1);
  f_wr->f_op = &pipe_wr_fops;
  f_wr->private_data = pipe;
  f_wr->f_mode = FMODE_WRITE;

  int fd0 = get_unused_fd_flags(0);
  int fd1 = get_unused_fd_flags(0);

  if (fd0 < 0 || fd1 < 0) {
    if (fd0 >= 0) put_unused_fd(fd0);
    if (fd1 >= 0) put_unused_fd(fd1);
    kfree(f_rd);
    kfree(f_wr);
    free_pipe_info(pipe);
    return -EMFILE;
  }

  fd_install(fd0, f_rd);
  fd_install(fd1, f_wr);

  pipefd[0] = fd0;
  pipefd[1] = fd1;
  return 0;
}
