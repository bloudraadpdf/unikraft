/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <signal.h>

#include <uk/essentials.h>
#include <uk/process.h>
#include <uk/syscall.h>

#include "signal.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL

UK_LLSYSCALL_R_DEFINE(int, rt_tgsigqueueinfo,
		      int, tgid, int, tid,
		      int, signum, siginfo_t *, siginfo)
{
	if (unlikely(!IS_VALID(signum)))
		return -EINVAL;

	if (unlikely(!siginfo))
		return -EFAULT;

	if (tgid <= 0)
		return -ESRCH;

	/* The si_code must be valid when targeting a
	 * different process.
	 */
	if (unlikely(siginfo->si_code != SI_QUEUE))
		return -EPERM;

	return pprocess_signal_thread_do(tgid, tid, signum, siginfo);
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
UK_LLSYSCALL_R_DEFINE(int, rt_tgsigqueueinfo,
		      int, tgid, int, tid,
		      int, signum, siginfo_t *, siginfo)
{
	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
