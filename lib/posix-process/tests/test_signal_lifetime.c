/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/config.h>

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <string.h>
#include <sys/types.h>
#include <uk/alloc.h>
#include <uk/process/events.h>
#include <uk/test.h>
#include "../process.h"
#include "../signal/signal.h"

#include "lifetime_allocator.h"

static void signal_thread_destroy(struct posix_thread *thread)
{
	struct pprocess_retired retired = {0};

	uk_mutex_lock(&pprocess_mutex);
	pprocess_signal_tdesc_detach_locked(thread, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	pprocess_signal_tdesc_destroy(thread);
}

static void signal_process_destroy(struct posix_process *process)
{
	struct pprocess_retired retired = {0};

	uk_mutex_lock(&pprocess_mutex);
	pprocess_signal_pdesc_detach_locked(process, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	pprocess_signal_pdesc_destroy(process);
}

static struct uk_signal *queued_signal_create(struct uk_alloc *allocator,
					      struct posix_process *owner,
					      struct uk_signal_queue *queue,
					      int signum)
{
	struct uk_signal *signal = uk_signal_alloc(allocator);

	if (signal) {
		signal->siginfo.si_signo = signum;
		uk_list_add_tail(&signal->list_head, &queue->list_head[signum]);
		uk_sigaddset(&queue->pending, signum);
		owner->signal->queued_count++;
	}
	return signal;
}

UK_TESTCASE(posix_signal_lifetime,
	    failed_actions_leave_a_destroyable_descriptor)
{
	struct lifetime_allocator state;
	struct posix_process process;
	int rc;

	lifetime_allocator_init(&state);
	process = (struct posix_process){._a = &state.allocator};
	rc = pprocess_signal_pdesc_alloc(&process);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		return;
	state.fail_at = state.calls + 1;
	rc = pprocess_signal_pdesc_init(&process);
	UK_TEST_EXPECT_SNUM_EQ(rc, -ENOMEM);
	signal_process_destroy(&process);
	UK_TEST_EXPECT_NULL(process.signal);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime, unpublished_thread_uses_owned_process)
{
	struct lifetime_allocator state;
	struct posix_process process;
	struct posix_thread thread;
	int rc;

	lifetime_allocator_init(&state);
	process = (struct posix_process){._a = &state.allocator};
	thread = (struct posix_thread){._a = &state.allocator,
				       .process = &process,
				       .tid = 0};
	rc = pprocess_signal_pdesc_alloc(&process);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		return;
	rc = pprocess_signal_pdesc_init(&process);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = pprocess_signal_tdesc_alloc(&thread);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	pprocess_signal_tdesc_init(&thread);
	signal_thread_destroy(&thread);
	UK_TEST_EXPECT_NULL(thread.signal);
out:
	signal_process_destroy(&process);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime, reparent_queues_keep_their_allocator_owner)
{
	struct lifetime_allocator first, second;
	struct pprocess_retired retired = {0};
	struct posix_process old, next;
	struct posix_thread thread;
	struct uk_signal *sig;
	int rc;

	lifetime_allocator_init(&first);
	lifetime_allocator_init(&second);
	old = (struct posix_process){._a = &first.allocator};
	next = (struct posix_process){._a = &second.allocator};
	thread = (struct posix_thread){._a = &first.allocator, .process = &old};
	rc = pprocess_signal_pdesc_alloc(&old);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		return;
	rc = pprocess_signal_pdesc_init(&old);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = pprocess_signal_pdesc_alloc(&next);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = pprocess_signal_pdesc_init(&next);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = pprocess_signal_tdesc_alloc(&thread);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	pprocess_signal_tdesc_init(&thread);
	sig = queued_signal_create(old._a, &old, &thread.signal->sigqueue,
				   SIGUSR1);
	UK_TEST_EXPECT_NOT_NULL(sig);
	if (!sig)
		goto out;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_signal_tdesc_reset_locked(&thread, &retired);
	UK_TEST_EXPECT_SNUM_EQ(old.signal->queued_count, 0);
	thread.process = &next;
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	sig = queued_signal_create(thread._a, &next, &thread.signal->sigqueue,
				   SIGUSR1);
	UK_TEST_EXPECT_NOT_NULL(sig);
	if (!sig)
		goto out;
	sig = queued_signal_create(next._a, &next, &thread.signal->sigqueue,
				   SIGUSR2);
	UK_TEST_EXPECT_NOT_NULL(sig);
	if (!sig)
		goto out;
	signal_thread_destroy(&thread);
	UK_TEST_EXPECT_SNUM_EQ(next.signal->queued_count, 0);
out:
	if (thread.signal)
		signal_thread_destroy(&thread);
	signal_process_destroy(&next);
	signal_process_destroy(&old);
	UK_TEST_EXPECT_SNUM_EQ(first.live, 0);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(second.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(first.locked_frees, 0);
	UK_TEST_EXPECT_SNUM_EQ(second.locked_frees, 0);
}

static int signal_process_create(struct posix_process *process)
{
	int rc = pprocess_signal_pdesc_alloc(process);

	return rc ? rc : pprocess_signal_pdesc_init(process);
}

static int signal_thread_create(struct posix_thread *thread)
{
	int rc = pprocess_signal_tdesc_alloc(thread);

	if (!rc)
		pprocess_signal_tdesc_init(thread);
	return rc;
}

enum signal_clone_failure {
	SIGNAL_CLONE_INHERIT = 1 << 0,
	SIGNAL_CLONE_ALLOCATION = 1 << 1,
	SIGNAL_CLONE_THREAD_DESCRIPTOR = 1 << 2,
	SIGNAL_CLONE_PROCESS_DESCRIPTOR = 1 << 3,
	SIGNAL_CLONE_MASK = 1 << 4,
	SIGNAL_CLONE_ACTIONS = 1 << 5,
	SIGNAL_CLONE_HANDLER = 1 << 6,
	SIGNAL_CLONE_LIVE = 1 << 7,
	SIGNAL_CLONE_LEAK = 1 << 8,
	SIGNAL_CLONE_LOCKED_FREE = 1 << 9,
};

static int clone_without_signal_allocation(__u64 flags)
{
	struct lifetime_allocator state;
	struct posix_process parent_process, child_process;
	struct posix_thread parent, child;
	struct uk_signal_pdesc *pd;
	struct uk_signal_tdesc *td;
	struct uk_sigaction *previous = NULL;
	size_t calls, live;
	int rc, failures = 0;

	lifetime_allocator_init(&state);
	parent_process = (struct posix_process){._a = &state.allocator};
	child_process = (struct posix_process){._a = &state.allocator};
	parent = (struct posix_thread){._a = &state.allocator,
				       .process = &parent_process};
	child = (struct posix_thread){._a = &state.allocator,
				      .process = flags & CLONE_THREAD
							 ? &parent_process
							 : &child_process};
	rc = signal_process_create(&parent_process);
	if (rc)
		goto out;
	rc = signal_thread_create(&parent);
	if (rc)
		goto out;
	if (!(flags & CLONE_THREAD)) {
		rc = signal_process_create(&child_process);
		if (rc)
			goto out;
	}
	rc = signal_thread_create(&child);
	if (rc)
		goto out;
	KERN_SIGACTION(&parent_process, SIGUSR1)->ks_handler = SIG_IGN;
	uk_sigaddset(&parent.signal->mask, SIGUSR2);
	pd = child.process->signal;
	td = child.signal;
	calls = state.calls;
	live = state.live;
	state.fail_at = calls + 1;
	uk_mutex_lock(&pprocess_mutex);
	rc = pprocess_signal_inherit_locked(&child, &parent, flags | CLONE_VM,
					    &previous);
	uk_mutex_unlock(&pprocess_mutex);
	if (previous)
		pprocess_signal_sigaction_release(previous);
	if (rc)
		failures |= SIGNAL_CLONE_INHERIT;
	if (state.calls != calls)
		failures |= SIGNAL_CLONE_ALLOCATION;
	if (child.signal != td)
		failures |= SIGNAL_CLONE_THREAD_DESCRIPTOR;
	if (child.process->signal != pd)
		failures |= SIGNAL_CLONE_PROCESS_DESCRIPTOR;
	if (!rc) {
		if (memcmp(&child.signal->mask, &parent.signal->mask,
			   sizeof(parent.signal->mask)))
			failures |= SIGNAL_CLONE_MASK;
		if ((child.process->signal->sigaction ==
		     parent_process.signal->sigaction) !=
		    !!(flags & CLONE_SIGHAND))
			failures |= SIGNAL_CLONE_ACTIONS;
		if (KERN_SIGACTION(child.process, SIGUSR1)->ks_handler !=
		    (flags & CLONE_CLEAR_SIGHAND ? SIG_DFL : SIG_IGN))
			failures |= SIGNAL_CLONE_HANDLER;
		if (state.live !=
		    live - ((flags & CLONE_SIGHAND) && !(flags & CLONE_THREAD)))
			failures |= SIGNAL_CLONE_LIVE;
	}
out:
	signal_thread_destroy(&child);
	signal_thread_destroy(&parent);
	signal_process_destroy(&child_process);
	signal_process_destroy(&parent_process);
	if (state.live)
		failures |= SIGNAL_CLONE_LEAK;
	if (state.locked_frees)
		failures |= SIGNAL_CLONE_LOCKED_FREE;
	return rc ? rc : failures;
}

UK_TESTCASE(posix_signal_lifetime, clone_copies_constructed_signal_state)
{
	UK_TEST_EXPECT_SNUM_EQ(clone_without_signal_allocation(0), 0);
}

UK_TESTCASE(posix_signal_lifetime, clone_shares_handlers_without_reallocation)
{
	UK_TEST_EXPECT_SNUM_EQ(clone_without_signal_allocation(CLONE_SIGHAND),
			       0);
}

UK_TESTCASE(posix_signal_lifetime, clone_clears_handlers_without_reallocation)
{
	UK_TEST_EXPECT_SNUM_EQ(clone_without_signal_allocation(
				       CLONE_CLEAR_SIGHAND),
			       0);
}

UK_TESTCASE(posix_signal_lifetime, clone_thread_retains_constructed_descriptor)
{
	UK_TEST_EXPECT_SNUM_EQ(clone_without_signal_allocation(CLONE_THREAD |
							       CLONE_SIGHAND),
			       0);
}

static struct posix_process *
signal_process_allocate(struct lifetime_allocator *state)
{
	struct posix_process *process =
		uk_zalloc(&state->allocator, sizeof(*process));
	int rc;

	if (!process)
		return NULL;
	process->_a = &state->allocator;
	uk_refcount_init(&process->references, 1);
	UK_INIT_LIST_HEAD(&process->threads);
	UK_INIT_LIST_HEAD(&process->children);
	rc = signal_process_create(process);
	if (rc) {
		signal_process_destroy(process);
		pprocess_put(process);
		return NULL;
	}
	return process;
}

UK_TESTCASE(posix_signal_lifetime, retired_thread_outlives_its_former_process)
{
	struct lifetime_allocator state;
	struct pprocess_retired threads = {0}, processes = {0};
	struct posix_process *process = NULL;
	struct posix_thread *thread = NULL;
	struct uk_signal *sig;
	size_t process_live, total_live;
	int rc;

	lifetime_allocator_init(&state);
	process = signal_process_allocate(&state);
	UK_TEST_EXPECT_NOT_NULL(process);
	if (!process)
		goto out;
	process_live = state.live;
	thread = uk_zalloc(&state.allocator, sizeof(*thread));
	UK_TEST_EXPECT_NOT_NULL(thread);
	if (!thread)
		goto out;
	thread->_a = &state.allocator;
	thread->process = process;
	rc = signal_thread_create(thread);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	sig = queued_signal_create(&state.allocator, process,
				   &thread->signal->sigqueue, SIGUSR1);
	UK_TEST_EXPECT_NOT_NULL(sig);
	if (!sig)
		goto out;
	total_live = state.live;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_retire_thread_locked(thread, &threads);
	UK_TEST_EXPECT_NULL(thread->process);
	UK_TEST_EXPECT_SNUM_EQ(process->signal->queued_count, 0);
	pprocess_retire_process_locked(process, &processes);
	UK_TEST_EXPECT_SNUM_EQ(state.live, total_live);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
	uk_mutex_unlock(&pprocess_mutex);
	thread = NULL;
	process = NULL;
	pprocess_retired_release(&processes);
	UK_TEST_EXPECT_SNUM_EQ(state.live, total_live - process_live);
	pprocess_retired_release(&threads);
out:
	if (thread) {
		signal_thread_destroy(thread);
		uk_free(&state.allocator, thread);
	}
	if (process) {
		signal_process_destroy(process);
		pprocess_put(process);
	}
	pprocess_retired_release(&processes);
	pprocess_retired_release(&threads);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime,
	    retained_proc_reference_survives_process_retirement)
{
	struct lifetime_allocator state;
	struct pprocess_retired retired = {0};
	struct posix_process *process;
	struct uk_signal *sig;
	size_t live;

	lifetime_allocator_init(&state);
	process = signal_process_allocate(&state);
	UK_TEST_EXPECT_NOT_NULL(process);
	if (!process)
		goto out;
	sig = queued_signal_create(&state.allocator, process,
				   &process->signal->sigqueue, SIGUSR1);
	UK_TEST_EXPECT_NOT_NULL(sig);
	if (!sig)
		goto out;
	pprocess_acquire(process);
	live = state.live;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_retire_process_locked(process, &retired);
	UK_TEST_EXPECT_SNUM_EQ(process->signal->queued_count, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.live, live);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_NULL(process->signal);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 1);
	pprocess_put(process);
	process = NULL;
out:
	if (process) {
		signal_process_destroy(process);
		pprocess_put(process);
	}
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime,
	    retained_signal_descriptor_owns_its_allocator)
{
	struct lifetime_allocator state;
	struct posix_process process;
	struct uk_signal_pdesc *descriptor;
	size_t live;
	int rc;

	lifetime_allocator_init(&state);
	process = (struct posix_process){._a = &state.allocator};
	rc = signal_process_create(&process);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc) {
		signal_process_destroy(&process);
		return;
	}
	descriptor = process.signal;
	pprocess_signal_pdesc_acquire(descriptor);
	live = state.live;
	signal_process_destroy(&process);
	UK_TEST_EXPECT_NULL(process.signal);
	UK_TEST_EXPECT_SNUM_EQ(state.live, live);
	process._a = NULL;
	pprocess_signal_pdesc_put(descriptor);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

struct lifetime_owned_signal {
	struct uk_signal signal;
	unsigned int dequeued, discarded, released, locked_releases;
};

static void lifetime_signal_dequeue_locked(struct uk_signal *signal,
					   enum uk_signal_dequeue_reason reason)
{
	struct lifetime_owned_signal *owned =
		__containerof(signal, struct lifetime_owned_signal, signal);

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (reason == UK_SIGNAL_CONSUME) {
		owned->dequeued++;
		signal->siginfo.si_overrun = 7;
	} else {
		owned->discarded++;
	}
}

static void lifetime_signal_release(struct uk_signal *signal)
{
	struct lifetime_owned_signal *owned =
		__containerof(signal, struct lifetime_owned_signal, signal);

	owned->released++;
	if (uk_mutex_is_locked(&pprocess_mutex))
		owned->locked_releases++;
}

static const struct uk_signal_ops lifetime_signal_ops = {
	.dequeue_locked = lifetime_signal_dequeue_locked,
	.release = lifetime_signal_release,
};

static struct lifetime_owned_signal lifetime_signal_create(void)
{
	return (struct lifetime_owned_signal){
		.signal = {.siginfo.si_signo = SIGUSR1,
			   .ops = &lifetime_signal_ops},
	};
}

static int signal_fixture_create(struct lifetime_allocator *state,
				 struct posix_process *process)
{
	lifetime_allocator_init(state);
	*process = (struct posix_process){._a = &state->allocator};
	UK_INIT_LIST_HEAD(&process->threads);
	return signal_process_create(process);
}

static int signal_pair_enqueue(struct posix_process *process,
			       struct lifetime_owned_signal *first,
			       struct lifetime_owned_signal *second)
{
	int rc = pprocess_signal_enqueue(process, NULL, &first->signal);

	if (rc)
		return rc;
	rc = pprocess_signal_enqueue(process, NULL, &second->signal);
	return rc;
}

UK_TESTCASE(posix_signal_lifetime,
	    owned_standard_notifications_keep_separate_loans)
{
	struct lifetime_allocator state;
	struct posix_process process;
	struct posix_thread thread;
	struct lifetime_owned_signal first = lifetime_signal_create();
	struct lifetime_owned_signal second = lifetime_signal_create();
	struct uk_signal *signal;
	uk_sigset_t mask;
	siginfo_t info;
	int rc;

	rc = signal_fixture_create(&state, &process);
	thread = (struct posix_thread){._a = &state.allocator,
				       .process = &process};

	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = signal_thread_create(&thread);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = signal_pair_enqueue(&process, &first, &second);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	UK_TEST_EXPECT_SNUM_EQ(process.signal->queued_count, 2);
	UK_TEST_EXPECT_SNUM_EQ(first.released + second.released, 0);
	uk_sigemptyset(&mask);
	uk_sigaddset(&mask, SIGUSR1);
	uk_mutex_lock(&pprocess_mutex);
	rc = pprocess_signal_take_locked(&thread, &mask, &info);
	UK_TEST_EXPECT_SNUM_EQ(rc, SIGUSR1);
	UK_TEST_EXPECT_SNUM_EQ(info.si_overrun, 7);
	UK_TEST_EXPECT_SNUM_EQ(first.dequeued, 1);
	UK_TEST_EXPECT_SNUM_EQ(first.released, 1);
	UK_TEST_EXPECT_SNUM_EQ(first.locked_releases, 1);
	UK_TEST_EXPECT(IS_PENDING(process.signal->sigqueue, SIGUSR1));
	uk_mutex_unlock(&pprocess_mutex);
	signal = pprocess_signal_dequeue(&process, NULL, SIGUSR1);
	UK_TEST_EXPECT(signal == &second.signal);
	UK_TEST_EXPECT_SNUM_EQ(second.dequeued, 1);
	uk_signal_free(signal);
	UK_TEST_EXPECT_SNUM_EQ(second.released, 1);
	UK_TEST_EXPECT_SNUM_EQ(second.locked_releases, 0);
	UK_TEST_EXPECT_SNUM_EQ(process.signal->queued_count, 0);
out:
	signal_thread_destroy(&thread);
	signal_process_destroy(&process);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime,
	    discarded_notification_returns_after_outer_unlock)
{
	struct lifetime_allocator state;
	struct posix_process process;
	struct pprocess_retired retired = {0};
	struct lifetime_owned_signal owned = lifetime_signal_create();
	int rc;

	rc = signal_fixture_create(&state, &process);

	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = pprocess_signal_enqueue(&process, NULL, &owned.signal);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	uk_mutex_lock(&pprocess_mutex);
	pprocess_signal_clear_pending(&process, SIGUSR1, &retired);
	UK_TEST_EXPECT_SNUM_EQ(owned.discarded, 1);
	UK_TEST_EXPECT_SNUM_EQ(owned.dequeued, 0);
	UK_TEST_EXPECT_SNUM_EQ(owned.released, 0);
	UK_TEST_EXPECT_SNUM_EQ(process.signal->queued_count, 0);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(owned.released, 1);
	UK_TEST_EXPECT_SNUM_EQ(owned.locked_releases, 0);
out:
	signal_process_destroy(&process);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

UK_TESTCASE(posix_signal_lifetime,
	    withdrawing_one_owner_preserves_another_pending_owner)
{
	struct lifetime_allocator state;
	struct posix_process process;
	struct pprocess_retired retired = {0};
	struct lifetime_owned_signal first = lifetime_signal_create();
	struct lifetime_owned_signal second = lifetime_signal_create();
	struct uk_signal *signal;
	int rc;

	rc = signal_fixture_create(&state, &process);

	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	rc = signal_pair_enqueue(&process, &first, &second);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);
	if (rc)
		goto out;
	uk_mutex_lock(&pprocess_mutex);
	UK_TEST_EXPECT(pprocess_signal_withdraw_locked(&process, NULL,
						       &second.signal,
						       &retired));
	UK_TEST_EXPECT(!pprocess_signal_withdraw_locked(&process, NULL,
							&second.signal,
							&retired));
	UK_TEST_EXPECT_SNUM_EQ(second.discarded, 1);
	UK_TEST_EXPECT_SNUM_EQ(second.dequeued, 0);
	UK_TEST_EXPECT_SNUM_EQ(second.released, 0);
	UK_TEST_EXPECT_SNUM_EQ(process.signal->queued_count, 1);
	UK_TEST_EXPECT(IS_PENDING(process.signal->sigqueue, SIGUSR1));
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(second.released, 1);
	UK_TEST_EXPECT_SNUM_EQ(second.locked_releases, 0);
	signal = pprocess_signal_dequeue(&process, NULL, SIGUSR1);
	UK_TEST_EXPECT(signal == &first.signal);
	uk_signal_free(signal);
	UK_TEST_EXPECT_SNUM_EQ(first.dequeued, 1);
	UK_TEST_EXPECT_SNUM_EQ(first.released, 1);
	UK_TEST_EXPECT_SNUM_EQ(process.signal->queued_count, 0);
out:
	signal_process_destroy(&process);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(state.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(state.locked_frees, 0);
}

uk_testsuite_rootfs_prio(posix_signal_lifetime, NULL, UK_PRIO_LATEST);
#endif
