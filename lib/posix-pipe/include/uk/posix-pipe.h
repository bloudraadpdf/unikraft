/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Support for POSIX pipes */

#ifndef __UKPOSIX_PIPE_H__
#define __UKPOSIX_PIPE_H__

#include <uk/file.h>
#include <uk/bitops.h>

#define UK_PIPE_CAPACITY UK_BIT(CONFIG_LIBPOSIX_PIPE_SIZE_ORDER)
#define UK_PIPE_READ_PEEK UK_BIT(30)
#define UK_PIPE_WRITE_ATOMIC UK_BIT(29)

int uk_pipefile_set_capacity(const struct uk_file *file, size_t capacity);
struct uk_file *uk_pipefile_budget_create(size_t capacity);
int uk_pipefile_budget_set(const struct uk_file *file, size_t capacity);
ssize_t uk_pipefile_write_packet(const struct uk_file *file,
				 const struct iovec *iov, size_t count,
				 const struct uk_file *budget);
void uk_pipefile_discard(const struct uk_file *file);

/* File creation */
int uk_pipefile_create(struct uk_file *pipes[2]);

/* Internal syscalls */
#if CONFIG_LIBPOSIX_FDTAB
int uk_sys_pipe(int pipefd[2], int flags);
#endif /* CONFIG_LIBPOSIX_FDTAB */

#endif /* __UKPOSIX_PIPE_H__ */
