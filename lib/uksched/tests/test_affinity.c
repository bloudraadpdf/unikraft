/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <uk/alloc_impl.h>
#include <uk/errptr.h>
#include <uk/sched.h>
#include <uk/test.h>
#include <uk/wait.h>

struct request_allocator {
	struct uk_alloc allocator;
	struct uk_waitq released;
	unsigned int live;
	bool fail;
};

static void *request_alloc(struct uk_alloc *allocator, size_t bytes)
{
	struct request_allocator *scope =
	    __containerof(allocator, struct request_allocator, allocator);
	void *memory =
	    scope->fail ? NULL : uk_malloc(uk_alloc_get_default(), bytes);

	if (memory)
		__atomic_add_fetch(&scope->live, 1, __ATOMIC_RELAXED);
	return memory;
}

static void request_free(struct uk_alloc *allocator, void *memory)
{
	struct request_allocator *scope =
	    __containerof(allocator, struct request_allocator, allocator);
	struct uk_waitq_ticket *ticket;
	unsigned long flags;

	uk_free(uk_alloc_get_default(), memory);
	_uk_waitq_lock(&scope->released, flags);
	__atomic_sub_fetch(&scope->live, 1, __ATOMIC_RELEASE);
	uk_list_for_each_entry(ticket, &scope->released.waiters, link)
		_uk_waitq_wake(&scope->released, ticket);
	_uk_waitq_unlock(&scope->released, flags);
}

static void request_allocator_init(struct request_allocator *scope)
{
	memset(scope, 0, sizeof(*scope));
	scope->allocator.malloc = request_alloc;
	scope->allocator.calloc = uk_calloc_compat;
	scope->allocator.free = request_free;
	uk_waitq_init(&scope->released);
}

static void request_released(struct request_allocator *scope)
{
	unsigned long flags;

	uk_waitq_wait_event(&scope->released,
			    __atomic_load_n(&scope->live, __ATOMIC_ACQUIRE) ==
				0);
	_uk_waitq_lock(&scope->released, flags);
	_uk_waitq_unlock(&scope->released, flags);
}

UK_TESTCASE(scheduler_affinity, failed_allocation_has_no_caller_owner)
{
	struct request_allocator scope;
	struct uk_sched_affinity *request;
	unsigned long mask[UK_THREAD_CPU_MASK_WORDS] = {0};

	request_allocator_init(&scope);
	scope.fail = true;
	request = uk_sched_affinity_create(&scope.allocator, mask);
	UK_TEST_EXPECT(PTRISERR(request));
	UK_TEST_EXPECT_SNUM_EQ(PTR2ERR(request), -ENOMEM);
	UK_TEST_EXPECT_NULL(uk_thread_current()->affinity_wait);
	UK_TEST_EXPECT_SNUM_EQ(scope.live, 0);
}

UK_TESTCASE(scheduler_affinity, same_owner_completion_releases_both_references)
{
	struct request_allocator scope;
	struct uk_sched_affinity *request;
	unsigned long mask[UK_THREAD_CPU_MASK_WORDS];
	int rc;

	request_allocator_init(&scope);
	rc = uk_sched_thread_get_affinity(uk_thread_current(), sizeof(mask),
					  mask);
	UK_TEST_EXPECT_SNUM_GT(rc, 0);
	if (rc < 0)
		return;
	request = uk_sched_affinity_create(&scope.allocator, mask);
	UK_TEST_EXPECT(!PTRISERR(request));
	if (PTRISERR(request))
		return;
	rc = uk_sched_thread_affinity_submit(uk_thread_current(), request);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (!rc) {
		rc = uk_sched_thread_affinity_submit(uk_thread_current(),
						     request);
		UK_TEST_EXPECT_SNUM_EQ(rc, -EALREADY);
		UK_TEST_EXPECT_SNUM_EQ(uk_sched_thread_affinity_wait(request),
				       0);
	} else {
		uk_sched_affinity_discard(request);
	}
	request_released(&scope);
	UK_TEST_EXPECT_NULL(uk_thread_current()->affinity_wait);
	UK_TEST_EXPECT_SNUM_EQ(scope.live, 0);
}

UK_TESTCASE(scheduler_affinity, empty_mask_preserves_task_ownership)
{
	struct request_allocator scope;
	struct uk_sched_affinity *request;
	unsigned long empty[UK_THREAD_CPU_MASK_WORDS] = {0};
	unsigned long before[UK_THREAD_CPU_MASK_WORDS],
	    after[UK_THREAD_CPU_MASK_WORDS];
	int rc;

	request_allocator_init(&scope);
	rc = uk_sched_thread_get_affinity(uk_thread_current(), sizeof(before),
					  before);
	UK_TEST_EXPECT_SNUM_GT(rc, 0);
	if (rc < 0)
		return;
	request = uk_sched_affinity_create(&scope.allocator, empty);
	UK_TEST_EXPECT(!PTRISERR(request));
	if (PTRISERR(request))
		return;
	rc = uk_sched_thread_affinity_submit(uk_thread_current(), request);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EINVAL);
	uk_sched_affinity_discard(request);
	request_released(&scope);
	rc = uk_sched_thread_get_affinity(uk_thread_current(), sizeof(after),
					  after);
	UK_TEST_EXPECT_SNUM_GT(rc, 0);
	UK_TEST_EXPECT_BYTES_EQ(before, after, rc > 0 ? (size_t)rc : 0);
	UK_TEST_EXPECT_SNUM_EQ(scope.live, 0);
	UK_TEST_EXPECT_NULL(uk_thread_current()->affinity_wait);
}

#if CONFIG_HAVE_SMP
static void mask_cpu(unsigned long *mask, unsigned int cpu)
{
	memset(mask, 0, UK_THREAD_CPU_MASK_WORDS * sizeof(*mask));
	mask[cpu / (sizeof(*mask) * CHAR_BIT)] =
	    1UL << (cpu % (sizeof(*mask) * CHAR_BIT));
}

static unsigned int another_cpu(const unsigned long *mask)
{
	unsigned int current = uk_sched_current()->cpu_index;

	for (unsigned int cpu = 0; cpu < CONFIG_UKPLAT_CPU_MAXCOUNT; cpu++)
		if (cpu != current &&
		    (mask[cpu / (sizeof(*mask) * CHAR_BIT)] &
		     (1UL << (cpu % (sizeof(*mask) * CHAR_BIT)))))
			return cpu;
	UK_CRASH("Affinity SMP units require at least two started CPUs\n");
}

static int set_current_mask(const unsigned long *mask)
{
	struct uk_sched_affinity *request =
	    uk_sched_affinity_create(uk_alloc_get_default(), mask);
	int rc;

	if (PTRISERR(request))
		return PTR2ERR(request);
	rc = uk_sched_thread_affinity_submit(uk_thread_current(), request);
	if (!rc)
		return uk_sched_thread_affinity_wait(request);
	uk_sched_affinity_discard(request);
	return rc;
}

static void __noreturn blocked_target_ran(void)
{
	UK_CRASH("Blocked affinity target executed\n");
}

static struct uk_thread *blocked_target(struct uk_sched *owner)
{
	struct uk_alloc *memory = uk_alloc_get_default();
	struct uk_thread *thread =
	    uk_thread_create_fn0(memory, blocked_target_ran, memory, 0, memory, 0,
				 memory, false, "affinity-target", NULL, NULL);

	if (!thread)
		UK_CRASH("Affinity target allocation failed\n");
	uk_thread_set_blocked(thread);
	if (uk_sched_thread_add(owner, thread))
		UK_CRASH("Affinity target attachment failed\n");
	return thread;
}

UK_TESTCASE(scheduler_affinity, stopped_target_cancels_queued_ownership)
{
	struct request_allocator scope;
	struct uk_sched_affinity *request;
	struct uk_thread *target;
	unsigned long original[UK_THREAD_CPU_MASK_WORDS],
	    mask[UK_THREAD_CPU_MASK_WORDS];
	int rc;

	request_allocator_init(&scope);
	rc = uk_sched_thread_get_affinity(uk_thread_current(), sizeof(original),
					  original);
	UK_TEST_EXPECT_SNUM_GT(rc, 0);
	if (rc < 0)
		return;
	mask_cpu(mask, another_cpu(original));
	target = blocked_target(uk_sched_current());
	request = uk_sched_affinity_create(&scope.allocator, mask);
	if (PTRISERR(request))
		UK_CRASH("Affinity test request allocation failed\n");
	rc = uk_sched_thread_affinity_submit(target, request);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		UK_CRASH("Affinity target submission failed\n");
	UK_TEST_EXPECT_SNUM_EQ(target->affinity_pending, 1);
	uk_sched_thread_remove(target);
	uk_thread_release(target);
	UK_TEST_EXPECT_SNUM_EQ(uk_sched_thread_affinity_wait(request), -ESRCH);
	request_released(&scope);
	UK_TEST_EXPECT_SNUM_EQ(scope.live, 0);
}

struct stopped_caller {
	struct request_allocator allocation;
	struct uk_waitq ready;
	struct uk_thread *target;
	unsigned long mask[UK_THREAD_CPU_MASK_WORDS];
	bool queued;
};

static void __noreturn pending_caller(void *argument)
{
	struct stopped_caller *test = argument;
	struct uk_sched_affinity *request =
	    uk_sched_affinity_create(&test->allocation.allocator, test->mask);
	struct uk_thread *current = uk_thread_current();

	if (PTRISERR(request) ||
	    uk_sched_thread_affinity_submit(test->target, request))
		UK_CRASH("Affinity queued caller setup failed\n");
	__atomic_store_n(&test->queued, true, __ATOMIC_RELEASE);
	uk_waitq_wake_up(&test->ready);
	while (!(__atomic_load_n(&current->flags, __ATOMIC_ACQUIRE) &
		 UK_THREADF_STOPPING))
		__asm__ __volatile__("" ::: "memory");
	uk_sched_yield();
	UK_CRASH("Stopped affinity caller resumed\n");
}

UK_TESTCASE(scheduler_affinity, queued_request_outlives_stopped_caller_stack)
{
	struct stopped_caller test = {0};
	struct uk_alloc *memory = uk_alloc_get_default();
	struct uk_sched *source = uk_sched_current();
	struct uk_thread *caller;
	unsigned long original[UK_THREAD_CPU_MASK_WORDS];
	int rc;

	request_allocator_init(&test.allocation);
	uk_waitq_init(&test.ready);
	rc = uk_sched_thread_get_affinity(uk_thread_current(), sizeof(original),
					  original);
	UK_TEST_EXPECT_SNUM_GT(rc, 0);
	if (rc < 0)
		return;
	mask_cpu(test.mask, another_cpu(original));
	test.target = blocked_target(source);
	rc = set_current_mask(test.mask);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		UK_CRASH("Affinity controller migration failed\n");
	caller = uk_thread_create_fn1(memory, pending_caller, &test, memory, 0,
				      memory, 0, memory, false, "affinity-caller",
				      NULL, NULL);
	if (!caller || uk_sched_thread_add(source, caller))
		UK_CRASH("Affinity caller attachment failed\n");
	uk_waitq_wait_event(&test.ready,
			    __atomic_load_n(&test.queued, __ATOMIC_ACQUIRE));
	uk_sched_thread_remove(caller);
	uk_thread_release(caller);
	request_released(&test.allocation);
	UK_TEST_EXPECT_SNUM_EQ(test.allocation.live, 0);
	uk_sched_thread_remove(test.target);
	uk_thread_release(test.target);
	UK_TEST_EXPECT_SNUM_EQ(set_current_mask(original), 0);
	UK_TEST_EXPECT_NULL(uk_thread_current()->affinity_wait);
}
#endif

UK_TESTSUITE_AT_APPINIT(scheduler_affinity, NULL);
