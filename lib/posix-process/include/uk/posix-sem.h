/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_POSIX_SEM_H
#define UK_POSIX_SEM_H

#include <uk/config.h>
#include <sys/types.h>

struct iovec;

#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
struct uk_posix_sem_limits {
	int per_set;
	int total;
	int operations;
	int sets;
};

void uk_posix_sem_limits_get(struct uk_posix_sem_limits *limits);
ssize_t uk_posix_sem_limits_write(const struct iovec *iov, size_t count,
				  size_t offset);
#endif

#endif
