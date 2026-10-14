/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <stddef.h>
#include <signal.h>
#include <sys/types.h>
#include <uk/syscall.h>

#include "signal.h"
#include "sigset.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
UK_LLSYSCALL_R_DEFINE(int, rt_sigsuspend,
		      const uk_sigset_t *, mask,
		      size_t, sigsetsize)
{
	struct posix_thread *thread = uk_pthread_current();
	struct uk_signal_tdesc *td = thread->signal;
	int rc;

	if (unlikely(sigsetsize != sizeof(uk_sigset_t)))
		return -EINVAL;
	if (unlikely(!mask))
		return -EFAULT;
	uk_mutex_lock(&pprocess_mutex);
	td->saved_mask = td->mask;
	td->restore_mask = true;
	td->mask = *mask;
	uk_sigdelset(&td->mask, SIGKILL);
	uk_sigdelset(&td->mask, SIGSTOP);
	rc = pprocess_signal_wait_locked(thread, NULL, NULL, 0);
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
UK_LLSYSCALL_R_DEFINE(int, rt_sigsuspend,
		      const uk_sigset_t *, mask,
		      size_t, sigsetsize)
{
	UK_WARN_STUBBED();
	return -EINTR;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
