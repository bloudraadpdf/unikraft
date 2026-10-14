/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <signal.h>
#include <stdlib.h>
#include <sys/time.h>

#include <uk/essentials.h>
#include <uk/syscall.h>
#include <uk/timeutil.h>

#include <uk/plat/time.h>
#if !CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/sched.h>
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#include "process.h"
#include "signal.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
UK_LLSYSCALL_R_DEFINE(int, rt_sigtimedwait,
		      const uk_sigset_t *, set,
		      siginfo_t *, info,
		      const struct timespec *, timeout,
		      size_t, sigsetsize)
{
	struct posix_thread *thread = uk_pthread_current();
	uk_sigset_t wait_set;
	__nsec deadline = 0;
	__nsec now, limit;
	int rc;

	if (unlikely(sigsetsize != sizeof(uk_sigset_t)))
		return -EINVAL;
	if (unlikely(!set))
		return -EFAULT;
	if (timeout) {
		if (unlikely(!(uk_time_spec_canonical(timeout) &&
			       uk_time_spec_positive(timeout))))
			return -EINVAL;
		now = ukplat_monotonic_clock();
		limit = (__NSEC_MAX >> 1) - now;
		deadline = (__u64)timeout->tv_sec > limit / 1000000000ULL ?
			limit : MIN(uk_time_spec_to_nsec(timeout), limit);
		deadline += now;
	}
	uk_sigcopyset(&wait_set, set);
	uk_sigdelset(&wait_set, SIGKILL);
	uk_sigdelset(&wait_set, SIGSTOP);
	uk_mutex_lock(&pprocess_mutex);
	rc = pprocess_signal_wait_locked(thread, &wait_set, info, deadline);
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
UK_LLSYSCALL_R_DEFINE(int, rt_sigtimedwait,
		      const uk_sigset_t *, set,
		      siginfo_t *, info,
		      const struct timespec *, timeout,
		      size_t, sigsetsize)
{
	struct uk_thread *current = uk_thread_current();
	__nsec timeout_ns;
	int rc;

	if (timeout) {
		if (unlikely(!(uk_time_spec_canonical(timeout) &&
			       uk_time_spec_positive(timeout))))
			return -EINVAL;
		timeout_ns = ukplat_monotonic_clock() +
			ukarch_time_sec_to_nsec(timeout->tv_sec) +
			timeout->tv_nsec;
		uk_thread_block_until(current, (__snsec)timeout_ns);
		uk_sched_yield();
		rc = -EAGAIN;
	} else {
		uk_thread_block(current);
		uk_sched_yield();
		/* If this ever returns then just return an EINTR to not
		 * confuse the application
		 */
		rc = -EINTR;
	}

	/* We currently never return a signal */
	*info = (siginfo_t){0};
	return rc;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
