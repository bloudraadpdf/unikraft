/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/alloc.h>
#include <uk/errptr.h>
#include <uk/sched.h>
#include <uk/test.h>
#include "../process.h"

static void __noreturn task_entry(void)
{
	uk_thread_exit();
}

static struct uk_thread *task_worker_create(const char *name)
{
	struct uk_alloc *allocator = uk_alloc_get_default();

	return uk_thread_create_fn0(allocator, task_entry, allocator, 0, NULL,
				    0, allocator, false, name, NULL, NULL);
}

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBPOSIX_PROCESS_SIGNAL
struct task_allocator {
	struct uk_alloc allocator;
	unsigned int live;
};

static void *task_calloc(struct uk_alloc *allocator, size_t count, size_t size)
{
	struct task_allocator *state = __containerof(allocator,
		struct task_allocator, allocator);
	void *memory = uk_calloc(uk_alloc_get_default(), count, size);

	if (memory)
		state->live++;
	return memory;
}

static void *task_malloc(struct uk_alloc *allocator __unused,
			 size_t size __unused)
{
	return NULL;
}

static void task_free(struct uk_alloc *allocator, void *memory)
{
	struct task_allocator *state = __containerof(allocator,
		struct task_allocator, allocator);

	state->live--;
	uk_free(uk_alloc_get_default(), memory);
}

UK_TESTCASE(posix_process_task, signal_allocation_failure_leaves_no_attachment)
{
	struct task_allocator state = {.allocator = {
		.calloc = task_calloc, .malloc = task_malloc, .free = task_free
	}};
	struct posix_process process = {._a = &state.allocator};
	struct uk_thread *thread = task_worker_create("task-rollback");
	struct posix_thread *attached;

	UK_TEST_EXPECT(thread && !PTRISERR(thread));
	if (!thread || PTRISERR(thread))
		return;
	attached = pprocess_create_pthread(&process, thread, NULL);
	UK_TEST_EXPECT(PTRISERR(attached));
	UK_TEST_EXPECT(PTR2ERR(attached) == -ENOMEM);
	UK_TEST_EXPECT(!uk_thread_uktls_var(thread, pthread_self));
	UK_TEST_EXPECT(state.live == 0);
	uk_thread_uktls_var(thread, pthread_self) = NULL;
	uk_thread_release(thread);
}
#endif

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBUKVMEM
struct cleanup_observation {
	unsigned int cleanups, exits;
	bool cleanup_locked, exit_locked, attached, vm_live, marked;
	bool cleanup_before_exit;
	int maps_result;
};

static __uk_tls struct cleanup_observation *cleanup_observation;

static int task_cleanup_observe(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct cleanup_observation *observation =
		uk_thread_uktls_var(event->thread, cleanup_observation);
	struct posix_thread *thread;
	struct uk_posix_vm_mapping *maps;
	size_t count;

	if (!observation)
		return UK_EVENT_NOT_HANDLED;
	observation->cleanups++;
	observation->cleanup_locked = uk_mutex_is_locked(&pprocess_mutex);
	thread = uk_thread_uktls_var(event->thread, pthread_self);
	observation->attached = thread && thread->process &&
		thread->tid == event->tid;
	observation->marked = thread && thread->cleanup_started;
	observation->vm_live = thread && thread->process &&
		thread->process->vm_context &&
		thread->process->vm_context->users;
	if (observation->vm_live && !observation->cleanup_locked) {
		struct uk_posix_vm_view *view =
			&thread->process->vm_context->view;

		observation->maps_result = uk_posix_vm_maps(view,
							    &maps, &count);
		if (!observation->maps_result)
			uk_posix_vm_maps_release(maps, count);
	}
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTCLEANUP_HANDLER_PRIO(task_cleanup_observe, UK_PRIO_LATEST);

static int task_exit_observe(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct cleanup_observation *observation =
		uk_thread_uktls_var(event->thread, cleanup_observation);

	if (!observation)
		return UK_EVENT_NOT_HANDLED;
	observation->cleanup_before_exit = observation->cleanups == 1;
	observation->exits++;
	observation->exit_locked = uk_mutex_is_locked(&pprocess_mutex);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTEXIT_HANDLER_PRIO(task_exit_observe, UK_PRIO_LATEST);

static struct posix_process *task_process_acquire(struct uk_thread *thread)
{
	struct posix_process *process =
		pprocess_create(uk_alloc_get_default(), thread, NULL);

	if (process && !PTRISERR(process))
		pprocess_acquire(process);
	return process;
}

static struct uk_thread *task_owner_create(const char *name,
					   struct posix_process **process)
{
	struct uk_thread *thread = task_worker_create(name);

	if (!thread || PTRISERR(thread))
		return thread;
	*process = task_process_acquire(thread);
	if (!*process || PTRISERR(*process)) {
		uk_thread_release(thread);
		return *process ? ERR2PTR(PTR2ERR(*process)) : NULL;
	}
	return thread;
}

static void task_process_put(struct posix_process *process)
{
	struct pprocess_retired retired = {0};

	uk_mutex_lock(&pprocess_mutex);
	if (pid2pprocess(process->pid) == process)
		pprocess_release(process, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	pprocess_put(process);
}

static void task_process_release(struct uk_thread *thread,
				 struct posix_process *process)
{
	pprocess_thread_fini(thread);
	uk_thread_uktls_var(thread, cleanup_observation) = NULL;
	uk_thread_release(thread);
	task_process_put(process);
}

UK_TESTCASE(posix_process_task, cleanup_keeps_target_vm_before_locked_exit)
{
	struct posix_process *process;
	struct uk_thread *thread = task_owner_create("cleanup-owner", &process);
	struct cleanup_observation observation = {.maps_result = -EDEADLK};

	UK_TEST_EXPECT(thread && !PTRISERR(thread));
	if (!thread || PTRISERR(thread))
		return;

	uk_thread_uktls_var(thread, cleanup_observation) = &observation;
	pprocess_thread_fini(thread);
	UK_TEST_EXPECT_SNUM_EQ(observation.cleanups, 1);
	UK_TEST_EXPECT_SNUM_EQ(observation.exits, 1);
	UK_TEST_EXPECT(!observation.cleanup_locked);
	UK_TEST_EXPECT(observation.exit_locked);
	UK_TEST_EXPECT(observation.cleanup_before_exit);
	UK_TEST_EXPECT(observation.attached && observation.vm_live);
	UK_TEST_EXPECT(observation.marked);
	UK_TEST_EXPECT_SNUM_EQ(observation.maps_result, 0);
	UK_TEST_EXPECT_NULL(uk_thread_uktls_var(thread, pthread_self));
	UK_TEST_EXPECT_NULL(process->vm_context);
	pprocess_thread_fini(thread);
	UK_TEST_EXPECT_SNUM_EQ(observation.cleanups, 1);
	UK_TEST_EXPECT_SNUM_EQ(observation.exits, 1);
	task_process_release(thread, process);
}

UK_TESTCASE(posix_process_task, vm_replace_cleans_old_vm_and_reopens_admission)
{
	struct posix_process *process;
	struct uk_thread *thread = task_owner_create("exec-cleanup", &process);
	struct posix_thread *target;
	struct posix_vm_context *old, *replacement;
	struct cleanup_observation observation = {.maps_result = -EDEADLK};

	UK_TEST_EXPECT(thread && !PTRISERR(thread));
	if (!thread || PTRISERR(thread))
		return;

	uk_thread_uktls_var(thread, cleanup_observation) = &observation;
	replacement = pprocess_vm_alloc(uk_alloc_get_default());
	UK_TEST_EXPECT_NOT_NULL(replacement);
	if (!replacement)
		goto out;
	uk_mutex_lock(&pprocess_mutex);
	old = process->vm_context;
	old->references++;
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_vm_replace(thread, replacement);
	UK_TEST_EXPECT(process->vm_context == replacement);
	UK_TEST_EXPECT_ZERO(old->users);
	UK_TEST_EXPECT(old->references == 1);
	UK_TEST_EXPECT(observation.cleanups == 1 && !observation.exits);
	UK_TEST_EXPECT(observation.attached && observation.vm_live);
	UK_TEST_EXPECT(observation.marked && !observation.cleanup_locked);
	UK_TEST_EXPECT_ZERO(observation.maps_result);
	target = uk_thread_uktls_var(thread, pthread_self);
	UK_TEST_EXPECT(target && !target->cleanup_started);
	uk_posix_vm_reference_put(old);
	pprocess_thread_fini(thread);
	UK_TEST_EXPECT(observation.cleanups == 2 && observation.exits == 1);
	UK_TEST_EXPECT(observation.exit_locked);
out:
	task_process_release(thread, process);
}

UK_TESTCASE(posix_process_task, release_without_auxstack_retires_task_metadata)
{
	struct posix_process *process;
	struct uk_thread *thread = task_owner_create("no-aux", &process);
	struct cleanup_observation observation = {0};

	UK_TEST_EXPECT(thread && !PTRISERR(thread));
	if (!thread || PTRISERR(thread))
		return;

	UK_TEST_EXPECT(!(thread->flags & UK_THREADF_AUXSP));
	uk_thread_uktls_var(thread, cleanup_observation) = &observation;
	uk_thread_release(thread);
	UK_TEST_EXPECT(observation.cleanups == 1 && observation.exits == 1);
	UK_TEST_EXPECT(observation.attached && observation.vm_live);
	UK_TEST_EXPECT(observation.cleanup_before_exit);
	UK_TEST_EXPECT_NULL(process->vm_context);
	task_process_put(process);
}

#endif

UK_TESTCASE(posix_process_task, raw_worker_has_no_task_metadata)
{
	struct uk_thread *thread = task_worker_create("raw-worker");
	char name[UK_POSIX_TASK_NAME_SIZE] = {0};
	struct posix_task candidate;
	int rc;

	UK_TEST_EXPECT(thread && !PTRISERR(thread));
	if (!thread || PTRISERR(thread))
		return;
	UK_TEST_EXPECT(pprocess_task_name_get(thread, name) == -ESRCH);
	UK_TEST_EXPECT(pprocess_task_name_set(thread, name) == -ESRCH);
	rc = pprocess_task_init(&candidate, thread, thread);
	UK_TEST_EXPECT(rc == -ESRCH);
	uk_thread_release(thread);
}

uk_testsuite_rootfs_prio(posix_process_task, NULL, UK_PRIO_LATEST);
