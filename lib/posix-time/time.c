/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * libnewlib glue code
 *
 * Authors: Felipe Huici <felipe.huici@neclab.eu>
 *          Florian Schmidt <florian.schmidt@neclab.eu>
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
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
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/times.h>
#include <uk/plat/time.h>
#include <uk/config.h>
#include <uk/print.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif
#include <uk/posix-time.h>
#include <uk/nofault.h>
#if CONFIG_LIBPOSIX_USER
#include <uk/credentials.h>
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#include <uk/posix-process-timer.h>
#endif

#if CONFIG_HAVE_SCHED
#include <uk/sched.h>
#include <uk/wait.h>
#else
#include <uk/lcpu.h>
#endif
#include <uk/essentials.h>

static __u64 realtime_offset;
#if CONFIG_HAVE_SCHED
static DEFINE_WAIT_QUEUE(clock_sleep_waiters);
static __u64 clock_sleep_generation;
#endif

static __nsec realtime_clock(void)
{
	return ukplat_wall_clock() +
		__atomic_load_n(&realtime_offset, __ATOMIC_RELAXED);
}

static void realtime_set_offset(__u64 offset)
{
	__atomic_store_n(&realtime_offset, offset, __ATOMIC_RELAXED);
#if CONFIG_HAVE_SCHED
	__atomic_add_fetch(&clock_sleep_generation, 1, __ATOMIC_RELEASE);
	uk_waitq_wake_up(&clock_sleep_waiters);
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	uk_posix_timers_clock_changed();
#endif
}

#if CONFIG_HAVE_SCHED
static bool sleep_woken(__u64 generation, const unsigned int *interrupted)
{
	if (__atomic_load_n(&clock_sleep_generation, __ATOMIC_ACQUIRE) !=
	    generation || (interrupted && uk_load_n(interrupted)))
		return true;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	return uk_posix_process_exit_requested();
#else
	return false;
#endif
}
#endif

static int sleep_wait(clockid_t clock, __nsec deadline)
{
	__nsec now, remaining, monotonic_deadline;
	int ret = 0;
#if CONFIG_HAVE_SCHED
	unsigned int *interrupted = NULL;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_posix_signal_wait signal;

	ret = uk_posix_signal_wait_init(&signal, &clock_sleep_waiters, NULL, 0);
	if (ret)
		return ret;
	interrupted = &signal.interrupted;
#endif
#endif
	for (;;) {
#if CONFIG_HAVE_SCHED
		__u64 generation = __atomic_load_n(&clock_sleep_generation,
						 __ATOMIC_ACQUIRE);
#endif

		now = clock == CLOCK_REALTIME ? realtime_clock()
					     : ukplat_monotonic_clock();
		if (now >= deadline)
			break;
#if CONFIG_HAVE_SCHED
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		ret = uk_posix_signal_wait_check(&signal);
		if (ret)
			break;
#elif CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		if (uk_posix_process_exit_requested()) {
			ret = -EINTR;
			break;
		}
#endif
#endif
		monotonic_deadline = deadline;
		if (clock == CLOCK_REALTIME) {
			remaining = deadline - now;
			now = ukplat_monotonic_clock();
			monotonic_deadline = remaining > INT64_MAX - now
					     ? INT64_MAX
					     : now + remaining;
		}
#if CONFIG_HAVE_SCHED
		uk_waitq_wait_event_deadline(&clock_sleep_waiters,
					     sleep_woken(generation, interrupted),
					     monotonic_deadline);
#else
		unsigned long flags = uk_lcpu_save_irqf();

		uk_lcpu_halt_irq_until(monotonic_deadline);
		uk_lcpu_restore_irqf(flags);
#endif
	}
#if CONFIG_HAVE_SCHED && CONFIG_LIBPOSIX_PROCESS_SIGNAL
	uk_posix_signal_wait_end(&signal, ret, UK_POSIX_SIGNAL_RESTART_NONE);
#endif
	return ret;
}

static int sleep_request(const struct timespec *request, __nsec *duration)
{
	struct timespec value;

	if (!uk_nofault_try_read(value, request))
		return -EFAULT;
	if (value.tv_sec < 0 || value.tv_nsec < 0 ||
	    value.tv_nsec >= UKARCH_NSEC_PER_SEC)
		return -EINVAL;
	if ((__u64)value.tv_sec >
	    ((__u64)INT64_MAX - (__u64)value.tv_nsec) / UKARCH_NSEC_PER_SEC)
		*duration = INT64_MAX;
	else
		*duration = ukarch_time_sec_to_nsec(value.tv_sec) + value.tv_nsec;
	return 0;
}

int uk_sys_nanosleep(const struct timespec *req, struct timespec *rem)
{
	__nsec before, after, deadline, nsec;
	struct timespec value;
	int ret = sleep_request(req, &nsec);

	if (ret)
		return ret;
	before = ukplat_monotonic_clock();
	deadline = nsec > INT64_MAX - before ? INT64_MAX : before + nsec;
	ret = sleep_wait(CLOCK_MONOTONIC, deadline);
	if (ret == -EINTR && rem) {
		after = ukplat_monotonic_clock();
		nsec = deadline > after ? deadline - after : 0;
		value.tv_sec = ukarch_time_nsec_to_sec(nsec);
		value.tv_nsec = ukarch_time_subsec(nsec);
		if (!uk_nofault_try_write(value, rem))
			return -EFAULT;
	}
	return ret;
}

UK_SYSCALL_R_DEFINE(int, nanosleep, const struct timespec*, req,
		    struct timespec*, rem)
{
	return uk_sys_nanosleep(req, rem);
}

#if UK_LIBC_SYSCALLS
int usleep(useconds_t usec)
{
	struct timespec ts;

	ts.tv_sec = (long int) (usec / 1000000);
	ts.tv_nsec = (long int) ukarch_time_usec_to_nsec(usec % 1000000);
	if (uk_sys_nanosleep(&ts, &ts))
		return -1;

	return 0;
}

unsigned int sleep(unsigned int seconds)
{
	struct timespec ts;

	ts.tv_sec = seconds;
	ts.tv_nsec = 0;
	if (uk_sys_nanosleep(&ts, &ts))
		return ts.tv_sec;

	return 0;
}
#endif /* UK_LIBC_SYSCALLS */

time_t uk_sys_time(time_t *tloc)
{
	time_t secs = ukarch_time_nsec_to_sec(realtime_clock());

	if (tloc)
		*tloc = secs;

	return secs;
}

UK_SYSCALL_R_DEFINE(time_t, time, time_t *, tloc)
{
	return uk_sys_time(tloc);
}

int uk_sys_gettimeofday(struct timeval *restrict tv, void *tz __unused)
{
	__nsec now = realtime_clock();

	if (!tv)
		return -EINVAL;

	tv->tv_sec = ukarch_time_nsec_to_sec(now);
	tv->tv_usec = ukarch_time_nsec_to_usec(ukarch_time_subsec(now));
	return 0;
}

UK_SYSCALL_R_DEFINE(int, gettimeofday, struct timeval *restrict, tv,
		    void *, tz)
{
	return uk_sys_gettimeofday(tv, tz);
}

int uk_sys_settimeofday(struct timeval *tv, void *tz)
{
	struct timespec tp;
	struct timeval value;

	if (tz)
		return -EOPNOTSUPP;
	if (!tv)
		return 0;
	if (!uk_nofault_try_read(value, tv))
		return -EFAULT;
	if (value.tv_usec < 0 || value.tv_usec >= 1000000)
		return -EINVAL;
	tp.tv_sec = value.tv_sec;
	tp.tv_nsec = value.tv_usec * 1000;
	return uk_sys_clock_settime(CLOCK_REALTIME, &tp);
}

UK_SYSCALL_R_DEFINE(int, settimeofday, struct timeval *, tv, void *, tz)
{
	return uk_sys_settimeofday(tv, tz);
}

int uk_sys_clock_getres(clockid_t clk_id, struct timespec *tp)
{
	const struct timespec value = {.tv_nsec = UKPLAT_TIME_TICK_NSEC};

	switch (clk_id) {
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_BOOTTIME:
#if CONFIG_HAVE_SCHED
	case CLOCK_THREAD_CPUTIME_ID:
#endif /* CONFIG_HAVE_SCHED */
		return !tp || uk_nofault_try_write(value, tp) ? 0 : -EFAULT;
	default:
		return -EINVAL;
	}
}

UK_SYSCALL_R_DEFINE(int, clock_getres, clockid_t, clk_id,
		    struct timespec *, tp)
{
	return uk_sys_clock_getres(clk_id, tp);
}

int uk_sys_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	__nsec now;
	struct timespec value;

	switch (clk_id) {
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_MONOTONIC_COARSE:
	case CLOCK_BOOTTIME:
		now = ukplat_monotonic_clock();
		break;
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_COARSE:
		now = realtime_clock();
		break;
#if CONFIG_HAVE_SCHED
	case CLOCK_THREAD_CPUTIME_ID:
		now = uk_sched_current_exec_time();
		break;
#endif /* CONFIG_HAVE_SCHED */
	default:
		return -EINVAL;
	}

	value.tv_sec = ukarch_time_nsec_to_sec(now);
	value.tv_nsec = ukarch_time_subsec(now);
	return uk_nofault_try_write(value, tp) ? 0 : -EFAULT;
}

UK_SYSCALL_R_DEFINE(int, clock_gettime, clockid_t, clk_id, struct timespec*, tp)
{
	return uk_sys_clock_gettime(clk_id, tp);
}

int uk_sys_clock_settime(clockid_t clk_id, const struct timespec *tp)
{
	struct timespec value;
	__u64 requested;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *credentials;
	bool allowed;
#endif

	if (clk_id != CLOCK_REALTIME)
		return -EINVAL;
	if (!uk_nofault_try_read(value, tp))
		return -EFAULT;
	if (value.tv_sec < 0 || value.tv_nsec < 0 ||
	    value.tv_nsec >= UKARCH_NSEC_PER_SEC)
		return -EINVAL;
	if ((__u64)value.tv_sec >
	    (UINT64_MAX - value.tv_nsec) / UKARCH_NSEC_PER_SEC)
		return -EOVERFLOW;
#if CONFIG_LIBPOSIX_USER
	credentials = pu_credentials_acquire(uk_thread_current(), NULL);
	if (!credentials)
		return -ESRCH;
	allowed = !!(pu_credentials_privileges(credentials)->effective &
		     PU_CAP_SYS_TIME);
	pu_credentials_release(credentials);
	if (!allowed)
		return -EPERM;
#endif
	requested = (__u64)value.tv_sec * UKARCH_NSEC_PER_SEC + value.tv_nsec;
	realtime_set_offset(requested - ukplat_wall_clock());
	return 0;
}

UK_SYSCALL_R_DEFINE(int, clock_settime, clockid_t, clk_id,
		    const struct timespec *, tp)
{
	return uk_sys_clock_settime(clk_id, tp);
}

int uk_sys_clock_nanosleep(clockid_t clockid, int flags,
			   const struct timespec *request,
			   struct timespec *remain)
{
	__nsec deadline;
	int result;

	switch (clockid) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		if (!(flags & TIMER_ABSTIME))
			return uk_sys_nanosleep(request, remain);
		result = sleep_request(request, &deadline);
		return result ? result : sleep_wait(clockid, deadline);
	case CLOCK_THREAD_CPUTIME_ID:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_MONOTONIC_COARSE:
		return -EOPNOTSUPP;
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_REALTIME_ALARM:
	case CLOCK_BOOTTIME_ALARM:
	case CLOCK_TAI:
		result = sleep_request(request, &deadline);
		return result ? result : -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

UK_SYSCALL_R_DEFINE(int, clock_nanosleep, clockid_t, clockid, int, flags,
		    const struct timespec *, request, struct timespec *, remain)
{
	return uk_sys_clock_nanosleep(clockid, flags, request, remain);
}

clock_t uk_sys_times(struct tms *buf)
{
	/* NOTE: We assume all system time is used by the current process */
	const clock_t utime = ukplat_monotonic_clock() /
			      (UKARCH_NSEC_PER_SEC / CLOCKS_PER_SEC);

	if (buf) {
		/* We don't track time spent in the kernel */
		buf->tms_stime = 0;
		buf->tms_utime = utime;
		/* We don't track time spent per-process; report zero */
		buf->tms_cutime = 0;
		buf->tms_cstime = 0;
	}
	return utime;
}

UK_SYSCALL_R_DEFINE(clock_t, times, struct tms *, buf)
{
	return uk_sys_times(buf);
}

UK_SYSCALL_R_DEFINE(int, setitimer, int, which,
		    const struct itimerval *, new_value,
		    struct itimerval *, old_value)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_setitimer(which, new_value, old_value);
#else
	return -EOPNOTSUPP;
#endif
}

UK_SYSCALL_R_DEFINE(int, getitimer, int, which, struct itimerval *, value)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	return uk_posix_getitimer(which, value);
#else
	return -EOPNOTSUPP;
#endif
}

#if CONFIG_LIBPOSIX_TIME_TEST
#include "tests/test_clock_sleep.c"
#endif
