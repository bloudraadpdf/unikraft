/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/config.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL && CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/test.h>
#include <uk/credentials.h>
#include <uk/plat/time.h>
#include <uk/posix-time.h>
#include "../signal/timer.h"

UK_TESTCASE(posix_timer_state, relative_realtime_deadline_stays_monotonic)
{
	struct posix_timer timer = {.clock = CLOCK_REALTIME};
	struct itimerspec value = {.it_value.tv_nsec = 5}, result;

	uk_mutex_lock(&pprocess_mutex);
	pprocess_timer_set_locked(&timer, 0, &value, 1000, 100);
	UK_TEST_EXPECT_SNUM_EQ(timer.effective_clock, CLOCK_MONOTONIC);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, 105);
	pprocess_timer_get_locked(&timer, &result, 2000, 101);
	UK_TEST_EXPECT_SNUM_EQ(result.it_value.tv_nsec, 4);
	pprocess_timer_set_locked(&timer, TIMER_ABSTIME, &value, 1000, 100);
	UK_TEST_EXPECT_SNUM_EQ(timer.effective_clock, CLOCK_REALTIME);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, 5);
	pprocess_timer_get_locked(&timer, &result, 2000, 101);
	UK_TEST_EXPECT_SNUM_EQ(result.it_value.tv_nsec, 1);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_TESTCASE(posix_timer_state,
	    elapsed_periods_advance_once_and_report_consumed_overrun)
{
	struct posix_timer timer = {
		.armed = true,
		.deadline = 10,
		.interval = 3,
		.overrun = 8,
	};
	int overrun;

	uk_mutex_lock(&pprocess_mutex);
	overrun = pprocess_timer_advance_locked(&timer, 21, UK_SIGNAL_DISCARD);
	UK_TEST_EXPECT_SNUM_EQ(overrun, 3);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, 22);
	UK_TEST_EXPECT_SNUM_EQ(timer.overrun, 8);
	overrun = pprocess_timer_advance_locked(&timer, 28, UK_SIGNAL_CONSUME);
	UK_TEST_EXPECT_SNUM_EQ(overrun, 2);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, 31);
	UK_TEST_EXPECT_SNUM_EQ(timer.overrun, 2);
	timer.deadline = 1;
	timer.interval = 1;
	overrun = pprocess_timer_advance_locked(&timer, (__u64)INT_MAX + 20,
						UK_SIGNAL_CONSUME);
	UK_TEST_EXPECT_SNUM_EQ(overrun, INT_MAX);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, (__u64)INT_MAX + 21);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_TESTCASE(posix_timer_state, rearm_invalidates_an_outstanding_notification)
{
	struct posix_timer timer = {
		.clock = CLOCK_MONOTONIC,
		.loaned = true,
		.queued = true,
		.notification_current = true,
		.overrun = 9,
	};
	struct itimerspec value = {.it_value.tv_nsec = 5}, zero = {0};

	uk_mutex_lock(&pprocess_mutex);
	pprocess_timer_set_locked(&timer, 0, &value, 1000, 100);
	UK_TEST_EXPECT(timer.loaned && timer.queued);
	UK_TEST_EXPECT(!timer.notification_current);
	UK_TEST_EXPECT_SNUM_EQ(timer.deadline, 105);
	UK_TEST_EXPECT_SNUM_EQ(timer.overrun, 0);
	pprocess_timer_set_locked(&timer, 0, &zero, 1000, 101);
	UK_TEST_EXPECT(!timer.armed);
	UK_TEST_EXPECT(timer.loaned);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_TESTCASE(posix_timer_state,
	    disarmed_real_interval_retains_its_supplied_period)
{
	struct posix_timer timer = {
		.clock = CLOCK_REALTIME,
		.real_interval = true,
	};
	struct itimerspec value = {.it_interval.tv_nsec = 3}, result;

	uk_mutex_lock(&pprocess_mutex);
	pprocess_timer_set_locked(&timer, 0, &value, 1000, 100);
	pprocess_timer_get_locked(&timer, &result, 1000, 100);
	UK_TEST_EXPECT(!timer.armed);
	UK_TEST_EXPECT_SNUM_EQ(result.it_value.tv_nsec, 0);
	UK_TEST_EXPECT_SNUM_EQ(result.it_interval.tv_nsec, 3);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_TESTCASE(posix_timer_state,
	    queued_expiry_is_consumed_after_realtime_moves_back)
{
	struct posix_timer timer = {
		.armed = true,
		.deadline = 100,
		.loaned = true,
		.notification_current = true,
	};

	uk_mutex_lock(&pprocess_mutex);
	pprocess_timer_advance_locked(&timer, 90, UK_SIGNAL_CONSUME);
	UK_TEST_EXPECT(!timer.armed);
	UK_TEST_EXPECT_SNUM_EQ(timer.overrun, 0);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_TESTCASE(posix_timer_state,
	    clock_set_import_precedes_value_and_capability_checks)
{
	struct timespec invalid = {.tv_sec = -1};

	UK_TEST_EXPECT_SNUM_EQ(uk_sys_clock_settime(CLOCK_MONOTONIC,
						    (const struct timespec *)1),
			       -EINVAL);
	UK_TEST_EXPECT_SNUM_EQ(uk_sys_clock_settime(CLOCK_REALTIME,
						    (const struct timespec *)1),
			       -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(uk_sys_clock_settime(CLOCK_REALTIME, &invalid),
			       -EINVAL);
}

UK_TESTCASE(posix_timer_state, missing_time_capability_cannot_change_realtime)
{
	struct pu_credentials *before, *replacement = NULL, *previous = NULL,
				       *removed;
	const struct pu_privileges *privileges;
	struct timespec value;
	__u64 monotonic, elapsed;
	int rc, result;

	before = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT_NOT_NULL(before);
	if (!before)
		return;
	privileges = pu_credentials_privileges(before);
	rc = pu_credentials_capabilities(uk_alloc_get_default(), before,
					 privileges->effective &
						 ~PU_CAP_SYS_TIME,
					 privileges->permitted,
					 privileges->inheritable, &replacement);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	monotonic = ukplat_monotonic_clock();
	rc = uk_sys_clock_gettime(CLOCK_REALTIME, &value);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	previous = pu_credentials_exchange(replacement);
	replacement = NULL;
	result = uk_sys_clock_settime(CLOCK_REALTIME, &value);
	removed = pu_credentials_exchange(previous);
	previous = NULL;
	pu_credentials_release(removed);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	if (!result) {
		elapsed = ukplat_monotonic_clock() - monotonic;
		value.tv_sec += elapsed / 1000000000ULL;
		value.tv_nsec += elapsed % 1000000000ULL;
		if (value.tv_nsec >= 1000000000L) {
			value.tv_sec++;
			value.tv_nsec -= 1000000000L;
		}
		UK_TEST_EXPECT_SNUM_EQ(uk_sys_clock_settime(CLOCK_REALTIME,
							    &value),
				       0);
	}
out:
	if (previous) {
		removed = pu_credentials_exchange(previous);
		pu_credentials_release(removed);
	}
	if (replacement)
		pu_credentials_release(replacement);
	pu_credentials_release(before);
}

uk_testsuite_rootfs_prio(posix_timer_state, NULL, UK_PRIO_LATEST);
#endif
