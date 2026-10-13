/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include <uk/process.h>
#include <uk/syscall.h>

#include "signal.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
UK_SYSCALL_R_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
{
	struct posix_thread *thread = uk_pthread_current();
	stack_t previous = thread->signal->altstack;

	if (ss) {
		stack_t next = *ss;
		unsigned int mode = (unsigned int)next.ss_flags & ~SS_AUTODISARM;

		if (previous.ss_flags & SS_ONSTACK)
			return -EPERM;
		if (mode != 0 && mode != SS_DISABLE && mode != SS_ONSTACK)
			return -EINVAL;
		if (mode == SS_DISABLE) {
			next.ss_sp = NULL;
			next.ss_size = 0;
		} else {
			if (next.ss_size < MINSIGSTKSZ)
				return -ENOMEM;
			next.ss_flags &= ~SS_ONSTACK;
		}
		thread->signal->altstack = next;
	}
	if (old_ss)
		*old_ss = previous;
	return 0;
}

#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */

UK_SYSCALL_R_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
{
	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
