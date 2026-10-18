/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <unistd.h>
#include <uk/essentials.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-process-timer.h>
#endif

UK_LLSYSCALL_R_DEFINE(long, alarm, unsigned int, seconds)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_alarm(seconds);
#else
	return -EOPNOTSUPP;
#endif
}

#if UK_LIBC_SYSCALLS
unsigned int alarm(unsigned int seconds)
{
	long result = uk_syscall_r_alarm((long)seconds);

	if (result < 0) {
		errno = -(int)result;
		return 0;
	}
	return (unsigned int)result;
}
#endif
