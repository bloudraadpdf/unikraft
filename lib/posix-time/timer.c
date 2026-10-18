/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Costin Lupu <costin.lupu@cs.pub.ro>
 *
 * Copyright (c) 2019, University Politehnica of Bucharest. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <time.h>
#include <signal.h>
#include <uk/config.h>
#include <uk/nofault.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-process-timer.h>
#endif

UK_LLSYSCALL_R_DEFINE(int, timer_create, clockid_t, clock, const void *, event,
		      int *, id)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_timer_create(clock, event, id);
#else
	return -EOPNOTSUPP;
#endif
}

UK_LLSYSCALL_R_DEFINE(int, timer_delete, int, id)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_timer_delete(id);
#else
	return -EOPNOTSUPP;
#endif
}

UK_LLSYSCALL_R_DEFINE(int, timer_settime, int, id, int, flags,
		      const struct itimerspec *, value, struct itimerspec *,
		      old)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_timer_settime(id, flags, value, old);
#else
	return -EOPNOTSUPP;
#endif
}

UK_LLSYSCALL_R_DEFINE(int, timer_gettime, int, id, struct itimerspec *, value)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_timer_gettime(id, value);
#else
	return -EOPNOTSUPP;
#endif
}

UK_LLSYSCALL_R_DEFINE(int, timer_getoverrun, int, id)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_timer_getoverrun(id);
#else
	return -EOPNOTSUPP;
#endif
}

#if UK_LIBC_SYSCALLS
static int timer_result(long result)
{
	if (result < 0) {
		errno = -(int)result;
		return -1;
	}
	return (int)result;
}

int timer_create(clockid_t clock, struct sigevent *restrict event,
		 timer_t *restrict id)
{
	struct sigevent owned;
	timer_t result;
	int kernel_id;
	long rc;

	if (event) {
		if (!uk_nofault_try_read(owned, event)) {
			errno = EFAULT;
			return -1;
		}
		if (owned.sigev_notify == SIGEV_THREAD) {
			errno = EOPNOTSUPP;
			return -1;
		}
	}
	rc = uk_syscall_r_timer_create((long)clock, event ? (long)&owned : 0,
				       (long)&kernel_id);
	if (timer_result(rc))
		return -1;
	result = (timer_t)(__uptr)kernel_id;
	if (!uk_nofault_try_write(result, id)) {
		uk_syscall_r_timer_delete((long)kernel_id);
		errno = EFAULT;
		return -1;
	}
	return 0;
}

int timer_delete(timer_t id)
{
	return timer_result(uk_syscall_r_timer_delete((long)(__uptr)id));
}

int timer_settime(timer_t id, int flags,
		  const struct itimerspec *restrict value,
		  struct itimerspec *restrict old)
{
	return timer_result(uk_syscall_r_timer_settime((long)(__uptr)id,
						       (long)flags, (long)value,
						       (long)old));
}

int timer_gettime(timer_t id, struct itimerspec *value)
{
	return timer_result(
		uk_syscall_r_timer_gettime((long)(__uptr)id, (long)value));
}

int timer_getoverrun(timer_t id)
{
	return timer_result(uk_syscall_r_timer_getoverrun((long)(__uptr)id));
}
#endif
