/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/futex.h>
#include <uk/test.h>
#include <uk/thread.h>

struct queue_fixture {
	struct uk_thread thread;
	struct uk_futex_key keys[2];
	struct uk_futex_waiter waiters[3];
};

static void queue_init(struct queue_fixture *fixture)
{
	fixture->keys[0].identity = fixture;
	fixture->keys[1] = fixture->keys[0];
	fixture->keys[1].kind = UK_FUTEX_KEY_VM;
	for (unsigned int index = 0; index < 3; index++) {
		fixture->waiters[index].thread = &fixture->thread;
		fixture->waiters[index].key = &fixture->keys[0];
		UK_INIT_LIST_HEAD(&fixture->waiters[index].link);
		UK_INIT_LIST_HEAD(&fixture->waiters[index].pi_link);
	}
}

static void queue_add(struct queue_fixture *fixture, unsigned int index,
		      bool pi, unsigned int priority)
{
	fixture->thread.sched_priority = priority;
	fixture->waiters[index].is_pi = pi;
	uk_futex_queue_add(&fixture->waiters[index]);
}

static void queue_finish(struct queue_fixture *fixture)
{
	for (unsigned int index = 0; index < 3; index++)
		uk_futex_queue_remove(&fixture->waiters[index]);
}

UK_TESTCASE(posix_futex_queue, mixed_waiters_keep_fifo_order)
{
	struct queue_fixture fixture = {0};

	queue_init(&fixture);
	queue_add(&fixture, 0, true, 0);
	queue_add(&fixture, 1, false, 0);
	queue_add(&fixture, 2, true, 0);
	UK_TEST_EXPECT(!uk_futex_pi_conflict(&fixture.keys[0]));
	uk_futex_queue_remove(&fixture.waiters[0]);
	UK_TEST_EXPECT(uk_futex_pi_conflict(&fixture.keys[0]));
	uk_futex_queue_remove(&fixture.waiters[1]);
	UK_TEST_EXPECT(!uk_futex_pi_conflict(&fixture.keys[0]));
	queue_finish(&fixture);
}

UK_TESTCASE(posix_futex_queue, priority_is_captured_before_fifo_ties)
{
	struct queue_fixture fixture = {0};

	queue_init(&fixture);
	queue_add(&fixture, 0, true, 5);
	queue_add(&fixture, 1, false, 10);
	fixture.thread.sched_priority = 0;
	UK_TEST_EXPECT(fixture.waiters[0].queue_priority == 5);
	UK_TEST_EXPECT(fixture.waiters[1].queue_priority == 10);
	UK_TEST_EXPECT(uk_futex_pi_conflict(&fixture.keys[0]));
	queue_add(&fixture, 2, true, 20);
	UK_TEST_EXPECT(!uk_futex_pi_conflict(&fixture.keys[0]));
	uk_futex_queue_remove(&fixture.waiters[2]);
	UK_TEST_EXPECT(uk_futex_pi_conflict(&fixture.keys[0]));
	queue_finish(&fixture);
}

UK_TESTCASE(posix_futex_queue, ordering_keeps_private_and_default_keys_separate)
{
	struct queue_fixture fixture = {0};

	queue_init(&fixture);
	fixture.waiters[0].key = &fixture.keys[1];
	queue_add(&fixture, 0, true, 0);
	queue_add(&fixture, 1, false, 0);
	UK_TEST_EXPECT(uk_futex_pi_conflict(&fixture.keys[0]));
	UK_TEST_EXPECT(!uk_futex_pi_conflict(&fixture.keys[1]));
	queue_add(&fixture, 2, true, 0);
	UK_TEST_EXPECT(uk_futex_pi_conflict(&fixture.keys[0]));
	uk_futex_queue_remove(&fixture.waiters[1]);
	UK_TEST_EXPECT(!uk_futex_pi_conflict(&fixture.keys[0]));
	queue_finish(&fixture);
}

uk_testsuite_register(posix_futex_queue, NULL);
