/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/test.h>

#define CLOCK_SLEEP_SECOND 1000000000ULL

struct clock_sleep_case {
	struct uk_waitq changed;
	struct timespec request;
	int flags, result;
	bool complete;
};

static void __noreturn clock_sleep_worker(void *argument)
{
	struct clock_sleep_case *test = argument;

	test->result = uk_sys_clock_nanosleep(CLOCK_REALTIME, test->flags,
					      &test->request, NULL);
	__atomic_store_n(&test->complete, true, __ATOMIC_RELEASE);
	uk_waitq_wake_up(&test->changed);
	for (;;) {
		uk_thread_block(uk_thread_current());
		uk_sched_yield();
	}
}

static struct uk_thread *clock_sleep_start(struct clock_sleep_case *test,
					   bool absolute)
{
	struct uk_alloc *memory = uk_alloc_get_default();
	struct uk_thread *thread;
	__nsec deadline = 60 * CLOCK_SLEEP_SECOND;

	*test = (struct clock_sleep_case){.flags = absolute ? TIMER_ABSTIME : 0};
	uk_waitq_init(&test->changed);
	if (absolute)
		deadline += realtime_clock();
	test->request = (struct timespec){
		.tv_sec = ukarch_time_nsec_to_sec(deadline),
		.tv_nsec = ukarch_time_subsec(deadline)};
	thread = uk_thread_create_fn1(memory, clock_sleep_worker, test, memory, 0,
				      memory, 0, memory, false, "clock-sleeper",
				      NULL, NULL);
	if (!thread)
		return NULL;
	if (uk_sched_thread_add(uk_sched_current(), thread)) {
		uk_thread_release(thread);
		return NULL;
	}
	uk_sched_yield();
	return thread;
}

static void clock_sleep_finish(struct uk_thread *thread, __u64 offset)
{
	uk_sched_thread_remove(thread);
	uk_thread_release(thread);
	realtime_set_offset(offset);
}

UK_TESTCASE(clock_sleep, forward_step_completes_absolute_realtime_wait)
{
	__u64 saved = __atomic_load_n(&realtime_offset, __ATOMIC_RELAXED);
	struct clock_sleep_case test;
	struct uk_thread *thread = clock_sleep_start(&test, true);
	bool queued;

	UK_TEST_EXPECT_NOT_NULL(thread);
	if (!thread)
		return;
	queued = thread->wait_ticket.wq == &clock_sleep_waiters;
	UK_TEST_EXPECT(queued);
	if (queued) {
		realtime_set_offset(saved + 120 * CLOCK_SLEEP_SECOND);
		uk_waitq_wait_event_deadline(&test.changed,
					     __atomic_load_n(&test.complete, __ATOMIC_ACQUIRE),
					     ukplat_monotonic_clock() + CLOCK_SLEEP_SECOND);
		UK_TEST_EXPECT(test.complete);
		UK_TEST_EXPECT_SNUM_EQ(test.result, 0);
	}
	clock_sleep_finish(thread, saved);
}

UK_TESTCASE(clock_sleep, clock_step_changes_only_absolute_deadlines)
{
	__u64 saved = __atomic_load_n(&realtime_offset, __ATOMIC_RELAXED);

	for (unsigned int absolute = 0; absolute < 2; absolute++) {
		struct clock_sleep_case test;
		struct uk_thread *thread = clock_sleep_start(&test, absolute);
		__nsec before;
		bool queued;

		UK_TEST_EXPECT_NOT_NULL(thread);
		if (!thread)
			return;
		queued = thread->wait_ticket.wq == &clock_sleep_waiters;
		UK_TEST_EXPECT(queued);
		if (queued) {
			before = thread->wakeup_time;
			realtime_set_offset(absolute
				? saved - 120 * CLOCK_SLEEP_SECOND
				: saved + 120 * CLOCK_SLEEP_SECOND);
			uk_sched_yield();
			UK_TEST_EXPECT(!test.complete);
			if (absolute) {
				UK_TEST_EXPECT_SNUM_GT(thread->wakeup_time,
						       before + 100 * CLOCK_SLEEP_SECOND);
			} else {
				UK_TEST_EXPECT_SNUM_EQ(thread->wakeup_time,
						       before);
			}
			UK_TEST_EXPECT(thread->wait_ticket.wq ==
				       &clock_sleep_waiters);
		}
		clock_sleep_finish(thread, saved);
	}
}

UK_TESTCASE(clock_sleep, retired_waiter_detaches_before_clock_publication)
{
	__u64 saved = __atomic_load_n(&realtime_offset, __ATOMIC_RELAXED);
	struct clock_sleep_case test;
	struct uk_thread *thread = clock_sleep_start(&test, true);

	UK_TEST_EXPECT_NOT_NULL(thread);
	if (!thread)
		return;
	UK_TEST_EXPECT(thread->wait_ticket.wq == &clock_sleep_waiters);
	uk_sched_thread_remove(thread);
	UK_TEST_EXPECT_NULL(thread->wait_ticket.wq);
	uk_thread_release(thread);
	realtime_set_offset(saved + 120 * CLOCK_SLEEP_SECOND);
	UK_TEST_EXPECT(__atomic_load_n(&realtime_offset, __ATOMIC_RELAXED) ==
		       saved + 120 * CLOCK_SLEEP_SECOND);
	realtime_set_offset(saved);
}

UK_TESTSUITE_AT_APPINIT(clock_sleep, NULL);
