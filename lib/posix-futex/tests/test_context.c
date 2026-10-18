/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/alloc.h>
#include <uk/errptr.h>
#include <uk/futex.h>
#include <uk/sched.h>
#include <uk/test.h>

struct context_fixture {
	struct uk_alloc allocator;
	struct uk_thread *thread;
	unsigned int live;
	bool fail, critical, freed_critical;
};

static void *context_calloc(struct uk_alloc *allocator, size_t count,
			    size_t size)
{
	struct context_fixture *fixture = __containerof(allocator,
		struct context_fixture, allocator);
	void *memory;

	if (fixture->fail)
		return NULL;
	memory = uk_calloc(uk_alloc_get_default(), count, size);
	if (memory)
		fixture->live++;
	return memory;
}

static void context_free(struct uk_alloc *allocator, void *memory)
{
	struct context_fixture *fixture = __containerof(allocator,
		struct context_fixture, allocator);

	fixture->freed_critical |= fixture->critical;
	fixture->live--;
	uk_free(uk_alloc_get_default(), memory);
}

static void __noreturn context_entry(void)
{
	uk_thread_exit();
}

static bool context_init(struct context_fixture *fixture)
{
	struct uk_alloc *allocator = uk_alloc_get_default();

	fixture->allocator.calloc = context_calloc;
	fixture->allocator.free = context_free;
	fixture->thread = uk_thread_create_fn0(allocator, context_entry,
					       allocator, 0, NULL, 0,
					       allocator, false,
					       "futex-context",
					       NULL, NULL);
	return fixture->thread && !PTRISERR(fixture->thread);
}

static void context_finish(struct context_fixture *fixture)
{
	uk_futex_call_abort_all(fixture->thread);
	uk_thread_release(fixture->thread);
}

static struct uk_futex_call *context_call(struct context_fixture *fixture)
{
	struct uk_futex_call *call;

	if (!context_init(fixture))
		return NULL;
	call = uk_futex_call_create(fixture->thread, &fixture->allocator);
	if (!call)
		context_finish(fixture);
	return call;
}

UK_TESTCASE(posix_futex_context, requeue_key_survives_its_callers_return)
{
	struct context_fixture fixture = {0};
	struct uk_futex_call *wait, *requeue, *retired = NULL;
	bool ready = context_init(&fixture);

	UK_TEST_EXPECT(ready);
	if (!ready)
		return;
	wait = uk_futex_call_create(fixture.thread, &fixture.allocator);
	requeue = uk_futex_call_create(fixture.thread, &fixture.allocator);
	UK_TEST_EXPECT(wait && requeue);
	if (!wait || !requeue)
		goto out;
	requeue->keys[1].offset = 48;
	uk_futex_waiter_key_set(&wait->waiter, wait, 0, &retired);
	uk_futex_waiter_key_set(&wait->waiter, requeue, 1, &retired);
	uk_futex_retired_release(retired);
	uk_futex_call_end(fixture.thread, requeue);
	UK_TEST_EXPECT(fixture.live == 2);
	UK_TEST_EXPECT(wait->waiter.key->offset == 48);
	uk_futex_call_end(fixture.thread, wait);
	UK_TEST_EXPECT_ZERO(fixture.live);
out:
	context_finish(&fixture);
}

UK_TESTCASE(posix_futex_context, target_cleanup_releases_nested_calls)
{
	struct context_fixture fixture = {0};
	struct uk_futex_call *outer, *inner, *retired = NULL;
	bool ready = context_init(&fixture);

	UK_TEST_EXPECT(ready);
	if (!ready)
		return;
	outer = uk_futex_call_create(fixture.thread, &fixture.allocator);
	inner = uk_futex_call_create(fixture.thread, &fixture.allocator);
	UK_TEST_EXPECT(outer && inner);
	if (outer && inner) {
		uk_futex_waiter_key_set(&inner->waiter, outer, 0, &retired);
		uk_futex_retired_release(retired);
	}
	uk_futex_call_abort_all(fixture.thread);
	UK_TEST_EXPECT_ZERO(fixture.live);
	context_finish(&fixture);
}

UK_TESTCASE(posix_futex_context, allocation_failure_preserves_the_active_chain)
{
	struct context_fixture fixture = {0};
	struct uk_futex_call *outer, *failed;
	bool ready = context_init(&fixture);

	UK_TEST_EXPECT(ready);
	if (!ready)
		return;
	outer = uk_futex_call_create(fixture.thread, &fixture.allocator);
	UK_TEST_EXPECT_NOT_NULL(outer);
	fixture.fail = true;
	failed = uk_futex_call_create(fixture.thread, &fixture.allocator);
	UK_TEST_EXPECT_NULL(failed);
	UK_TEST_EXPECT(fixture.live == (outer ? 1 : 0));
	uk_futex_call_abort_all(fixture.thread);
	UK_TEST_EXPECT_ZERO(fixture.live);
	context_finish(&fixture);
}

UK_TESTCASE(posix_futex_context, last_reference_defers_resource_release)
{
	struct context_fixture fixture = {0};
	struct uk_futex_call *call = context_call(&fixture);
	struct uk_futex_call *retired = NULL;

	UK_TEST_EXPECT_NOT_NULL(call);
	if (!call)
		return;
	uk_futex_call_get(call);
	uk_futex_call_end(fixture.thread, call);
	fixture.critical = true;
	uk_futex_call_put(call, &retired);
	UK_TEST_EXPECT(fixture.live == 1);
	fixture.critical = false;
	uk_futex_retired_release(retired);
	UK_TEST_EXPECT_ZERO(fixture.live);
	UK_TEST_EXPECT(!fixture.freed_critical);
	context_finish(&fixture);
}

UK_TESTCASE(posix_futex_context, raw_thread_release_drains_waits_and_calls)
{
	struct context_fixture fixture = {0};
	struct uk_futex_call *call = context_call(&fixture);
	struct uk_futex_call *retired = NULL;

	UK_TEST_EXPECT_NOT_NULL(call);
	if (!call)
		return;
	call->keys[0].identity = &fixture;
	uk_futex_waiter_key_set(&call->waiter, call, 0, &retired);
	uk_futex_retired_release(retired);
	uk_futex_queue_add(&call->waiter);
	uk_thread_release(fixture.thread);
	fixture.thread = NULL;
	UK_TEST_EXPECT_ZERO(fixture.live);
}

uk_testsuite_register(posix_futex_context, NULL);
