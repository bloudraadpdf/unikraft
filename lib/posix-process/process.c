/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
 * Copyright (c) 2022, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/config.h>
#include <sys/types.h>
#include <stddef.h>
#include <errno.h>
#include <uk/syscall.h>
#include <uk/sched.h>
#include <uk/alloc.h>
#include <uk/errptr.h>
#include <uk/pcpuvar.h>
#include <uk/nofault.h>
#include <uk/plat/config.h>
#include <limits.h>
#if CONFIG_LIBPOSIX_USER
#include <uk/credentials.h>
#endif
#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#endif

#include "scheduling.h"

struct uk_thread *pprocess_thread_main;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <signal.h> /* SIGCHLD */

#include <uk/bitops/bitmap.h>
#include <uk/list.h>
#include <uk/thread.h>
#include <uk/init.h>
#include <uk/essentials.h>
#include <uk/process.h>

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#include "process.h"

/**
 * System global lists
 * NOTE: We pre-allocate PID/TID 0 which is reserved by the kernel.
 *       An application should never get PID/TID 0 assigned.
 */
struct uk_posix_task {
	__atomic references;
	struct uk_posix_task *retired_next;
	pid_t number;
	__u64 serial;
	bool retired;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *credentials;
	bool no_new_privs;
#endif
	char name[UK_POSIX_TASK_NAME_SIZE];
};

static struct uk_posix_task *tid_identity[TIDMAP_SIZE];
static __u64 task_serial;
static struct posix_thread *tid_thread[TIDMAP_SIZE];
static unsigned long tid_map[UK_BITS_TO_LONGS(TIDMAP_SIZE)] = { [0] = 0x01UL };

/* Process Table */
struct posix_process *pid_process[TIDMAP_SIZE];

/**
 * Thread-local posix_thread reference
 */
__uk_tls struct posix_thread *pthread_self = NULL;

/**
 * Helpers to find and reserve a `pid_t`
 */
static inline pid_t find_free_tid(void)
{
	static pid_t prev = 0;
	unsigned long found;

	/* search starting from last position */
	found = uk_find_next_zero_bit(tid_map, TIDMAP_SIZE, prev + 1);
	if (found == TIDMAP_SIZE) {
		/* search again starting from the beginning */
		found = uk_find_first_zero_bit(tid_map, TIDMAP_SIZE);
	}
	if (found == TIDMAP_SIZE) {
		/* no free PID */
		uk_pr_err("Could not allocate TID: Out of TIDs (configured max: %d)\n",
			  CONFIG_LIBPOSIX_PROCESS_MAX_PID);
		return -1;
	}

	prev = found;
	return found;
}

void pprocess_task_put(struct uk_posix_task *task)
{
	if (task && uk_refcount_release(&task->references)) {
#if CONFIG_LIBPOSIX_USER
		if (task->credentials)
			pu_credentials_release(task->credentials);
#endif
		uk_free(uk_alloc_get_default(), task);
	}
}

struct uk_posix_task *pprocess_task_get_locked(struct posix_process *process,
					       pid_t tid)
{
	struct posix_thread *thread;
	struct uk_posix_task *identity;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (tid <= 0 || tid >= TIDMAP_SIZE ||
	    pid2pprocess(process->pid) != process)
		return NULL;
	thread = tid_thread[tid];
	if (!thread || thread->process != process ||
	    thread->state >= POSIX_THREAD_EXITED)
		return NULL;
	identity = tid_identity[tid];
	UK_ASSERT(identity);
	uk_refcount_acquire(&identity->references);
	return identity;
}

struct posix_thread *
pprocess_task_resolve_locked(struct posix_process *process,
			     struct uk_posix_task *identity)
{
	struct posix_thread *thread;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (!identity || identity->number <= 0 ||
	    identity->number >= TIDMAP_SIZE ||
	    tid_identity[identity->number] != identity ||
	    pid2pprocess(process->pid) != process)
		return NULL;
	thread = tid_thread[identity->number];
	return thread && thread->process == process &&
			       thread->state < POSIX_THREAD_EXITED
		       ? thread
		       : NULL;
}

static pid_t find_and_reserve_tid(void)
{
	struct uk_posix_task *task =
		uk_zalloc(uk_alloc_get_default(), sizeof(*task));
	pid_t tid;

	if (!task)
		return -ENOMEM;
	uk_refcount_init(&task->references, 1);
	uk_mutex_lock(&pprocess_mutex);
	tid = task_serial == UINT64_MAX ? -EOVERFLOW : find_free_tid();
	if (tid > 0) {
		uk_set_bit(tid, tid_map);
		task->number = tid;
		task->serial = ++task_serial;
		tid_identity[tid] = task;
		task = NULL;
	} else if (tid != -EOVERFLOW) {
		tid = -EAGAIN;
	}
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_task_put(task);
	return tid;
}

void pprocess_retire_thread_locked(struct posix_thread *thread,
				   struct pprocess_retired *retired)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_tdesc_detach_locked(thread, retired);
#endif
	thread->process = NULL;
	thread->retired_next = retired->threads;
	retired->threads = thread;
}

void pprocess_retire_process_locked(struct posix_process *process,
				    struct pprocess_retired *retired)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	UK_ASSERT(uk_list_empty(&process->threads));
	UK_ASSERT(uk_list_empty(&process->children));
	UK_ASSERT(!process->vm_context);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_pdesc_detach_locked(process, retired);
#endif
	process->parent = NULL;
	process->retired_next = retired->processes;
	retired->processes = process;
}

void pprocess_retired_release(struct pprocess_retired *retired)
{
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	while (retired->tasks) {
		struct uk_posix_task *task = retired->tasks;

		retired->tasks = task->retired_next;
		pprocess_task_put(task);
	}
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	while (retired->signals) {
		struct uk_signal *sig = retired->signals;

		retired->signals = sig->retired_next;
		uk_signal_free(sig);
	}
#endif
	while (retired->vms) {
		struct posix_vm_context *state = retired->vms;

		retired->vms = state->retired_next;
		pprocess_vm_destroy_retired(state);
	}
	while (retired->threads) {
		struct posix_thread *thread = retired->threads;

		retired->threads = thread->retired_next;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		pprocess_signal_tdesc_destroy(thread);
#endif
		uk_free(thread->_a, thread);
	}
	while (retired->processes) {
		struct posix_process *process = retired->processes;

		retired->processes = process->retired_next;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		pprocess_signal_pdesc_destroy(process);
#endif
		pprocess_put(process);
	}
}

static void release_tid(pid_t tid, struct pprocess_retired *retired)
{
	struct uk_posix_task *task = NULL;

	UK_ASSERT(tid > 0 && tid <= CONFIG_LIBPOSIX_PROCESS_MAX_PID);
	uk_mutex_lock(&pprocess_mutex);
	if (!tid_thread[tid] && !pid_process[tid]) {
		task = tid_identity[tid];
		tid_identity[tid] = NULL;
		uk_clear_bit(tid, tid_map);
		if (task) {
			task->retired_next = retired->tasks;
			retired->tasks = task;
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
}

/* Allocate a thread for a process */
struct posix_thread *pprocess_create_pthread(struct posix_process *pprocess,
					     struct uk_thread *th,
					     struct uk_thread *creator)
{
	struct posix_thread *pthread;
	struct pprocess_retired retired = {0};
	struct uk_alloc *a;
	pid_t tid;
	int err;

	UK_ASSERT(pprocess);
	UK_ASSERT(pprocess->_a);

	/* Take allocator from process */
	a = pprocess->_a;

	tid = find_and_reserve_tid();
	if (unlikely(tid < 0)) {
		err = tid;
		goto err_out;
	}

	pthread = uk_zalloc(a, sizeof(*pthread));
	if (unlikely(!pthread)) {
		err = -ENOMEM;
		goto err_free_tid;
	}

	pthread->_a = a;
	pthread->process = pprocess;
	pthread->tid = tid;
	pthread->thread = th;

	err = pprocess_task_init(&pthread->task, th, creator);
	if (err) {
		uk_free(a, pthread);
		goto err_free_tid;
	}

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	err = pprocess_signal_tdesc_alloc(pthread);
	if (unlikely(err)) {
		uk_pr_err("Could not allocate signal descriptor\n");
		/* Handle rollback manually to avoid adding more
		 * signal conditionals.
		 */
		uk_free(a, pthread);
		goto err_free_tid;
	}
	pprocess_signal_tdesc_init(pthread);
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

	/* Store reference to pthread with TID */
	uk_mutex_lock(&pprocess_mutex);
	uk_thread_uktls_var(th, pthread_self) = pthread;
	tid_thread[tid] = pthread;

	if (pprocess->exit_state != POSIX_PROCESS_RUNNING)
		pthread->exit_state = pprocess->exit_state == POSIX_PROCESS_KILLED ?
			POSIX_THREAD_KILLED : POSIX_THREAD_EXITED;
	else if (pprocess->exec_owner)
		pthread->exit_state = POSIX_THREAD_EXITED;

	/* Add to parent's list of threads */
	uk_list_add_tail(&pthread->thread_list_entry, &pprocess->threads);
	uk_mutex_unlock(&pprocess_mutex);

	uk_pr_debug("Process PID %d: New thread TID %d\n",
		    (int) pprocess->pid, (int) pthread->tid);
	return pthread;

err_free_tid:
	release_tid(tid, &retired);
	pprocess_retired_release(&retired);
err_out:
	return ERR2PTR(err);
}

/* Free thread that is part of a process
 * NOTE: The process is not released here when its thread list
 *       becomes empty.
 */
void pprocess_release_pthread(struct posix_thread *pthread,
			      struct pprocess_retired *retired)
{
	struct uk_posix_task *identity;

	UK_ASSERT(pthread);
	UK_ASSERT(pthread->_a);
	UK_ASSERT(pthread->process);

	uk_pr_debug("pid %d: Release tid %d\n",
		    pthread->process->pid, pthread->tid);

	uk_mutex_lock(&pprocess_mutex);
	uk_thread_uktls_var(pthread->thread, pthread_self) = NULL;
	for (int i = 1; i < TIDMAP_SIZE; ++i)
		if (tid_thread[i] && tid_thread[i]->vfork_parent == pthread)
			tid_thread[i]->vfork_parent = NULL;

	/* remove from process' thread list */
	uk_list_del_init(&pthread->thread_list_entry);
	uk_waitq_wake_up(&pthread->process->thread_wait);

	/* release TID */
	identity = tid_identity[pthread->tid];
	memcpy(identity->name, pthread->task.name,
	       UK_POSIX_TASK_NAME_SIZE);
	identity->retired = true;
#if CONFIG_LIBPOSIX_USER
	UK_ASSERT(!identity->credentials);
	identity->credentials = pu_credentials_acquire(pthread->thread,
						       &identity->no_new_privs);
	UK_ASSERT(identity->credentials);
#endif
	tid_thread[pthread->tid] = NULL;
	release_tid(pthread->tid, retired);
	pprocess_retire_thread_locked(pthread, retired);
	uk_mutex_unlock(&pprocess_mutex);
}

void pprocess_complete_vfork(struct posix_thread *pthread)
{
	struct posix_thread *parent;

	uk_mutex_lock(&pprocess_mutex);
	parent = pthread->vfork_parent;
	pthread->vfork_parent = NULL;
	if (parent && parent->state == POSIX_THREAD_BLOCKED_VFORK) {
		parent->state = POSIX_THREAD_RUNNING;
		uk_thread_wake(parent->thread);
	}
	uk_mutex_unlock(&pprocess_mutex);
}

/* Create a new posix process for a given thread */
struct posix_process *pprocess_create(struct uk_alloc *a,
				      struct uk_thread *thread,
				      struct uk_thread *parent)
{
	struct posix_thread  *parent_pthread  = NULL;
	struct posix_process *parent_pprocess = NULL;
	struct posix_thread  **pthread;
	struct posix_thread  *_pthread;
	struct posix_process *pprocess;
	struct posix_process *orig_pprocess;
	struct pprocess_retired retired = {0};
	int ret;

	/* Retrieve a reference to the `pthread_self` pointer on the remote TLS:
	 * Allows us changing the pointer value.
	 */
	pthread = &uk_thread_uktls_var(thread, pthread_self);

	if (parent)
		parent_pthread = uk_thread_uktls_var(parent, pthread_self);
	if (parent_pthread) {
		/* if we have a parent pthread,
		 *  it must have a surrounding pprocess
		 */
		UK_ASSERT(parent_pthread->process);

		parent_pprocess = parent_pthread->process;
	}

	/* Allocate pprocess structure */
	pprocess = uk_zalloc(a, sizeof(*pprocess));
	if (!pprocess) {
		ret = -ENOMEM;
		goto err_out;
	}
	pprocess->_a = a;
	uk_refcount_init(&pprocess->references, 1);
	UK_INIT_LIST_HEAD(&pprocess->threads);
	UK_INIT_LIST_HEAD(&pprocess->children);

	pprocess->state = POSIX_PROCESS_RUNNING;
	pprocess_resource_init(pprocess, parent_pprocess);

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	ret = pprocess_signal_pdesc_alloc(pprocess);
	if (unlikely(ret)) {
		uk_pr_err("Could not allocate signal descriptor\n");
		/* Free manually as we can jump to err_free_pprocess
		 * after this allocation is successful.
		 */
		uk_free(a, pprocess);
		goto err_out;
	}
	ret = pprocess_signal_pdesc_init(pprocess);
	if (unlikely(ret)) {
		uk_pr_err("Could not initialize signal descriptor\n");
		goto err_free_pprocess;
	}
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

	ret = pprocess_vm_init(pprocess, parent_pprocess);
	if (ret)
		goto err_free_pprocess;

	uk_waitq_init(&pprocess->child_wait);
	uk_waitq_init(&pprocess->thread_wait);
	uk_semaphore_init(&pprocess->exit_semaphore, 0);

	/* Check if we have a pthread structure already for this thread
	 * or if we need to allocate one
	 */
	if (!(*pthread)) {
		_pthread = pprocess_create_pthread(pprocess, thread, parent);
		if (PTRISERR(_pthread)) {
			ret = PTR2ERR(_pthread);
			goto err_free_pprocess;
		}
		/* take thread id for process id */
		pprocess->pid = _pthread->tid;
		*pthread = _pthread;
	} else {
		/* Re-use existing pthread, re-link it and re-use its TID */
		UK_ASSERT((*pthread)->thread == thread);

		/* Remove thread from original process */
		uk_mutex_lock(&pprocess_mutex);
		orig_pprocess = (*pthread)->process;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		pprocess_signal_tdesc_reset_locked(*pthread, &retired);
#endif
		uk_list_del(&(*pthread)->thread_list_entry);

		/* Re-assign thread to current process */
		(*pthread)->process = pprocess;
		pprocess->pid = (*pthread)->tid;
		uk_list_add_tail(&(*pthread)->thread_list_entry,
				 &pprocess->threads);

		/* Release original process if it became empty of threads */
		if (uk_list_empty(&orig_pprocess->threads))
			pprocess_release(orig_pprocess, &retired);
		uk_mutex_unlock(&pprocess_mutex);
		pprocess_retired_release(&retired);
	}

	/* Add to process table. No failure past this point. */
	if (unlikely((unsigned long)pprocess->pid >= ARRAY_SIZE(pid_process))) {
		uk_pr_err("Process limit reached, could not create new process\n");
		ret = -EAGAIN;
		goto err_free_pprocess;
	}

	uk_mutex_lock(&pprocess_mutex);
	pprocess->parent = parent_pprocess;
	if (parent_pprocess) {
		uk_list_add_tail(&pprocess->child_list_entry,
				 &parent_pprocess->children);
	}
	pid_process[pprocess->pid] = pprocess;
	uk_mutex_unlock(&pprocess_mutex);

	uk_pr_debug("Process PID %d created (parent PID: %d)\n",
		    (int) pprocess->pid,
		    (int) ((pprocess->parent) ? pprocess->parent->pid : 0));
	return pprocess;

err_free_pprocess:
	pprocess_vm_release_user(pprocess->vm_context, pprocess->pid);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_pdesc_destroy(pprocess);
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */
	uk_free(a, pprocess);
err_out:
	return ERR2PTR(ret);
}

static inline int raise_process_clone_event(struct uk_thread *child,
					    struct uk_thread *parent)
{
	struct posix_process_clone_event_data clone_data;
	struct clone_args cl_args;
	pid_t child_tid, child_pid;
	pid_t parent_tid, parent_pid;
	int ret;

	UK_ASSERT(child);

	child_tid = ukthread2tid(child);
	child_pid = ukthread2pid(child);

	if (parent) {
		parent_tid = ukthread2tid(parent);
		parent_pid = ukthread2pid(parent);
	} else {
		parent_tid = 0;
		parent_pid = 0;
	}

	/* We pass the flags used when creating a new process
	 * i.e. CLONE_VM | CLONE_VFORK | SIGCHLD.
	 */
	cl_args = (struct clone_args) {
		.flags       = CLONE_VM | CLONE_VFORK,
		.child_tid   = child_tid,
		.parent_tid  = parent_tid,
		.exit_signal = SIGCHLD,
	};

	clone_data = (struct posix_process_clone_event_data) {
		.cl_args = &cl_args,
		.cl_args_len = sizeof(cl_args),
		.child = child,
		.parent = parent,
		.ppid = parent_pid,
		.pid = child_pid,
		.tid = child_tid,
	};

	ret = pprocess_raise_clone_event(&clone_data);
	if (unlikely(ret < 0)) {
		uk_pr_err("clone event error (%d)\n", ret);
		return ret;
	}

	return 0;
}

#if CONFIG_LIBPOSIX_PROCESS_MULTIPROCESS
pid_t uk_posix_process_run(uk_posix_process_mainlike_func fn,
			   int argc, const char *argv[])
{
	struct posix_process_execve_event_data execve_data;
	struct posix_process *child_process;
	struct posix_vm_context *vm_new = NULL;
	struct uk_sched *s = uk_sched_current();
	struct uk_thread *parent;
	struct uk_thread *child;
	pid_t pid;
	int ret;

	UK_ASSERT(s);

	parent = uk_thread_current();
	UK_ASSERT(ukthread2tid(parent) > 0);

	/* Create container thread */
	child = uk_thread_create_container(uk_alloc_get_default(),
					   s->a_stack,
					   STACK_SIZE,
					   s->a_auxstack, 0, s->a_uktls,
					   false, "application", NULL, NULL);
	if (unlikely(!child)) {
		uk_pr_err("Could not create thread\n");
		return -ENOMEM;
	}

	/* Create new process */
	child_process = pprocess_create(uk_alloc_get_default(), child, parent);
	if (unlikely(PTRISERR(child_process))) {
		ret = PTR2ERR(child_process);
		uk_pr_err("Could not create process (%d)\n", ret);
		goto err_free_thread;
	}

	ret = raise_process_clone_event(child, parent);
	if (unlikely(ret)) {
		uk_pr_err("clone event error (%d)\n", ret);
		goto err_free_thread;
	}

	vm_new = pprocess_vm_alloc(child_process->_a);
	if (!vm_new) {
		ret = -ENOMEM;
		goto err_free_thread;
	}
	vm_new->stack_size = STACK_SIZE;

	/* Raise the execve event */
	execve_data = (struct posix_process_execve_event_data) {
		.thread = child,
		.tid = ukthread2tid(child),
		.pid = ukthread2pid(child),
	};
	ret = pprocess_raise_execve_event(&execve_data);
	if (unlikely(ret < 0)) {
		uk_pr_err("exeve event error (%d)\n", ret);
		goto err_free_thread;
	}

	pprocess_vm_replace(child, vm_new);
	vm_new = NULL;

	/* Schedule the process */
	uk_thread_container_init_fn2(child,
				     (uk_thread_fn2_t)fn,
				     (void *)(unsigned long)argc,
				     (void *)argv);
	pid = ukthread2pid(child);
	ret = uk_sched_thread_add(s, child);
	if (unlikely(ret < 0)) {
		uk_pr_err("Unable to add tid %d to scheduler (%d)\n",
			  ukthread2tid(child), ret);
		goto err_free_thread;
	}

	return pid;

err_free_thread:
	pprocess_vm_release_user(vm_new, ukthread2pid(child));
	/* also issues exit events */
	uk_thread_release(child);

	return ret;
}
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTIPROCESS */

void pprocess_acquire(struct posix_process *process)
{
	uk_refcount_acquire(&process->references);
}

void pprocess_put(struct posix_process *process)
{
	if (uk_refcount_release(&process->references))
		uk_free(process->_a, process);
}

/* Releases pprocess memory and other resources.
 * NOTE: All pthreads must be removed already
 *       from this pprocess. All chilren must
 *       be already reparented.
 */
void pprocess_release(struct posix_process *pprocess,
		      struct pprocess_retired *retired)
{
	pid_t pid;

	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(pprocess);
	UK_ASSERT(uk_list_empty(&pprocess->threads));
	UK_ASSERT(uk_list_empty(&pprocess->children));

	/* Unlink this process from its parent */
	if (pprocess->parent) {
		uk_list_del(&pprocess->child_list_entry);
		uk_waitq_wake_up(&pprocess->parent->child_wait);
	}

	pprocess_vm_retire_locked(pprocess->vm_context, pprocess->pid, retired);
	pprocess->vm_context = NULL;
	pid = pprocess->pid;

	pid_process[pid] = NULL;
	release_tid(pid, retired);

	pprocess_retire_process_locked(pprocess, retired);
	uk_mutex_unlock(&pprocess_mutex);

	uk_pr_debug("pid %d released\n", pid);
}

static int posix_process_init(struct uk_init_ctx *ictx)
{
	struct pprocess_retired retired = {0};
	struct posix_process *p;
	struct uk_thread *t;
	int ret;

	UK_ASSERT(ictx);

	/* If ictx->tmain in set, main() executes on a
	 * separate uk_thread. Instantiate PID_INIT from
	 * that thread, and set pprocess_thread_main, as
	 * we will need that information later.
	 */
	if (ictx->tmain) {
		t = ictx->tmain;
		pprocess_thread_main = ictx->tmain;
	} else {
		t = uk_thread_current();
	}

	/* Create a POSIX process without parent ("init" process) */
	p = pprocess_create(uk_alloc_get_default(), t, NULL);
	if (unlikely(PTRISERR(p))) {
		uk_pr_err("Could not create process (%d)\n", PTR2ERR(p));
		return PTR2ERR(p);
	}

	ret = raise_process_clone_event(t, NULL);
	if (unlikely(ret))
		pprocess_release(p, &retired);
	pprocess_retired_release(&retired);

	return ret;
}

uk_late_initcall(posix_process_init, 0x0);

void pprocess_thread_cleanup(struct uk_thread *thread)
{
	struct posix_process_ptexit_event_data cleanup;
	struct posix_thread *pthread;
	int ret __maybe_unused;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	pthread = uk_thread_uktls_var(thread, pthread_self);
	if (!pthread) {
		uk_mutex_unlock(&pprocess_mutex);
		return;
	}
	UK_ASSERT(thread == uk_thread_current() || !thread->sched);
	pthread->cleanup_started = true;
	cleanup = (struct posix_process_ptexit_event_data){
		.thread = thread, .tid = pthread->tid
	};
	uk_mutex_unlock(&pprocess_mutex);
	ret = uk_raise_event(POSIX_PROCESS_PTCLEANUP_EVENT, &cleanup);
	UK_ASSERT(ret >= 0 && ret != UK_EVENT_HANDLED);
}

/* Thread release: Release TID and posix_thread */
void pprocess_thread_fini(struct uk_thread *thread)
{
	struct pprocess_retired retired = {0};
	struct posix_process *pprocess;
	struct posix_thread *pthread;
	bool is_init;

	pprocess_thread_cleanup(thread);

	uk_mutex_lock(&pprocess_mutex);
	pthread = uk_thread_uktls_var(thread, pthread_self);

	/* No pthread was ever assigned to this uk_thread,
	 * or the pthread was already terminated as a result
	 * to an exit syscall or a signal.
	 *
	 * If the pthread exists, this is its return path.
	 */
	if (!pthread)
		goto out;

	pprocess = pthread->process;
	UK_ASSERT(pprocess);
	is_init = pprocess->pid == UK_PID_INIT;

	if (uk_list_is_singular(&pprocess->threads)) {
		pprocess_finish_exit(pprocess, pthread, &retired);
		if (is_init)
			pprocess_release(pprocess, &retired);
	} else {
		pprocess_exit_pthread(pthread,
				      pthread->exit_state == POSIX_THREAD_KILLED
					  ? POSIX_THREAD_KILLED
					  : POSIX_THREAD_EXITED,
				      &retired);
	}

out:
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
}

UK_THREAD_INIT_PRIO_FLAGS(0, pprocess_thread_fini,
			  UK_PRIO_AFTER(UK_PRIO_EARLIEST),
			  UK_THREAD_INITF_UKTLS);

struct posix_process *pid2pprocess(pid_t pid)
{
	if (pid < 0 || (__sz)pid >= ARRAY_SIZE(pid_process))
		return NULL;

	return pid_process[pid];
}

struct posix_thread *tid2pthread(pid_t tid)
{
	if ((__sz)tid >= ARRAY_SIZE(tid_thread) || tid < 0)
		return NULL;
	return tid_thread[tid];
}

struct posix_process *tid2pprocess(pid_t tid)
{
	struct posix_thread *pthread;

	pthread = tid2pthread(tid);
	if (!pthread)
		return NULL;

	return pthread->process;
}

struct uk_thread *tid2ukthread(pid_t tid)
{
	struct posix_thread *pthread;

	pthread = tid2pthread(tid);
	if (!pthread)
		return NULL;

	return pthread->thread;
}

pid_t ukthread2tid(struct uk_thread *thread)
{
	struct posix_thread *pthread;

	UK_ASSERT(thread);

	pthread = uk_thread_uktls_var(thread, pthread_self);
	if (!pthread)
		return -ENOTSUP;

	return pthread->tid;
}

pid_t ukthread2pid(struct uk_thread *thread)
{
	struct posix_thread *pthread;

	UK_ASSERT(thread);

	pthread = uk_thread_uktls_var(thread, pthread_self);
	if (!pthread)
		return -ENOTSUP;

	UK_ASSERT(pthread->process);

	return pthread->process->pid;
}

pid_t uk_sys_getpid(void)
{
	if (!pthread_self)
		return -ENOTSUP;

	UK_ASSERT(pthread_self->process);
	return pthread_self->process->pid;
}

pid_t uk_sys_gettid(void)
{
	if (!pthread_self)
		return -ENOTSUP;

	return pthread_self->tid;
}

/* PID of parent process  */
pid_t uk_sys_getppid(void)
{
	if (!pthread_self)
		return -ENOTSUP;

	UK_ASSERT(pthread_self->process);

	if (!pthread_self->process->parent) {
		 /* no parent, return 0 */
		return 0;
	}

	return pthread_self->process->parent->pid;
}
#else  /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#define UNIKRAFT_PID      1
#define UNIKRAFT_TID      1
#define UNIKRAFT_PPID     0

pid_t uk_sys_getpid(void)
{
	return UNIKRAFT_PID;
}

pid_t uk_sys_gettid(void)
{
	return UNIKRAFT_TID;
}

pid_t uk_sys_getppid(void)
{
	return UNIKRAFT_PPID;
}

#endif /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

UK_SYSCALL_R_DEFINE(pid_t, gettid)
{
	return uk_sys_gettid();
}

UK_SYSCALL_R_DEFINE(pid_t, getppid)
{
	return uk_sys_getppid();
}

UK_SYSCALL_R_DEFINE(pid_t, getpid)
{
	return uk_sys_getpid();
}

#if CONFIG_LIBPOSIX_USER
int pprocess_schedule_authorize(struct uk_thread *target,
				struct pu_credentials **caller,
				struct pu_credentials **credentials)
{
	const struct pu_identity *source, *identity;

	*caller = pu_credentials_acquire(uk_thread_current(), NULL);
	*credentials = pu_credentials_acquire(target, NULL);
	if (!*caller || !*credentials)
		return -ESRCH;
	source = pu_credentials_identity(*caller);
	identity = pu_credentials_identity(*credentials);
	if (source->effective_uid == identity->real_uid ||
	    source->effective_uid == identity->effective_uid ||
	    (pu_credentials_privileges(*caller)->effective & PU_CAP_SYS_NICE))
		return 0;
	return -EPERM;
}
#endif

static struct uk_thread *affinity_target_locked(pid_t pid)
{
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *thread =
		pid ? tid2pthread(pid) : uk_pthread_current();

	return thread ? thread->thread : NULL;
#else
	return !pid || pid == uk_sys_gettid() ? uk_thread_current() : NULL;
#endif
}

UK_SYSCALL_R_DEFINE(int, sched_setaffinity, int, pid, unsigned int, cpusetsize,
		    const unsigned long *, mask)
{
	struct uk_sched_affinity *request;
	unsigned long value[UK_THREAD_CPU_MASK_WORDS] = {0};
	struct uk_thread *target;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *caller = NULL, *credentials = NULL;
#endif
	size_t bytes = MIN((size_t)cpusetsize, sizeof(value));
	int rc;

	if (bytes && uk_nofault_memcpy((char *)value, (const char *)mask, bytes,
				       0) != bytes)
		return -EFAULT;
	request = uk_sched_affinity_create(uk_alloc_get_default(), value);
	if (PTRISERR(request))
		return PTR2ERR(request);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_lock(&pprocess_mutex);
#endif
	target = affinity_target_locked(pid);
	if (!target) {
		rc = -ESRCH;
		goto out;
	}
#if CONFIG_LIBPOSIX_USER
	rc = pprocess_schedule_authorize(target, &caller, &credentials);
	if (rc)
		goto out;
#endif
	rc = uk_sched_thread_affinity_submit(target, request);
out:
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_unlock(&pprocess_mutex);
#endif
#if CONFIG_LIBPOSIX_USER
	if (credentials)
		pu_credentials_release(credentials);
	if (caller)
		pu_credentials_release(caller);
#endif
	if (!rc)
		rc = uk_sched_thread_affinity_wait(request);
	else
		uk_sched_affinity_discard(request);
	return rc;
}

UK_SYSCALL_R_DEFINE(int, sched_getaffinity, int, pid, unsigned int, cpusetsize,
		    unsigned long *, mask)
{
	unsigned long value[UK_THREAD_CPU_MASK_WORDS];
	struct uk_thread *target;
	int ret;

	ret = uk_sched_get_cpu_mask(MIN((size_t)cpusetsize, sizeof(value)),
				    value);
	if (ret < 0)
		return ret;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_lock(&pprocess_mutex);
#endif
	target = affinity_target_locked(pid);
	ret = target ? uk_sched_thread_get_affinity(target, sizeof(value),
						    value)
		     : -ESRCH;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_unlock(&pprocess_mutex);
#endif
	if (ret > 0 && uk_nofault_memcpy((char *)mask, (const char *)value, ret,
					 0) != (size_t)ret)
		return -EFAULT;
	return ret;
}

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBPOSIX_VFS
struct posix_process *uk_posix_process_get(pid_t pid)
{
	struct posix_process *process;

	uk_mutex_lock(&pprocess_mutex);
	process = pid2pprocess(pid);
	if (process)
		pprocess_acquire(process);
	uk_mutex_unlock(&pprocess_mutex);
	return process;
}

void uk_posix_process_put(struct posix_process *process)
{
	pprocess_put(process);
}

bool uk_posix_process_attached(struct posix_process *process)
{
	bool attached;

	uk_mutex_lock(&pprocess_mutex);
	attached = pid2pprocess(process->pid) == process;
	uk_mutex_unlock(&pprocess_mutex);
	return attached;
}

static bool process_task_attached(struct posix_process *process,
				  struct uk_posix_task *identity)
{
	struct posix_thread *task;

	if (!identity || tid_identity[identity->number] != identity ||
	    pid2pprocess(process->pid) != process)
		return false;
	task = tid2pthread(identity->number);
	return task ? task->process == process :
		identity->number == process->pid && identity->retired;
}

#if CONFIG_LIBPOSIX_FDTAB
struct uk_fdtab *uk_posix_process_task_fdtable(struct posix_process *process,
					       struct uk_posix_task *identity)
{
	struct posix_thread *thread;
	struct uk_fdtab *table = NULL;

	uk_mutex_lock(&pprocess_mutex);
	thread = pprocess_task_resolve_locked(process, identity);
	if (thread)
		table = uk_fdtab_get_thread(thread->thread);
	uk_mutex_unlock(&pprocess_mutex);
	return table;
}
#endif

static char *process_task_name_buffer(struct posix_process *process,
				      struct uk_posix_task *identity)
{
	struct posix_thread *task;

	if (!process_task_attached(process, identity))
		return NULL;
	task = tid2pthread(identity->number);
	return task ? task->task.name : identity->name;
}

pid_t uk_posix_process_next_tid(struct posix_process *process, pid_t after)
{
	pid_t next = 0;

	if (after >= TIDMAP_SIZE - 1)
		return 0;
	uk_mutex_lock(&pprocess_mutex);
	if (pid2pprocess(process->pid) == process) {
		for (pid_t tid = MAX(after, 0) + 1; tid < TIDMAP_SIZE; tid++) {
			struct posix_thread *task = tid_thread[tid];

			if ((task && task->process == process) ||
			    (!task && tid == process->pid &&
			     tid_identity[tid] && tid_identity[tid]->retired)) {
				next = tid;
				break;
			}
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	return next;
}

struct uk_posix_task *uk_posix_process_task_get(struct posix_process *process,
						pid_t tid)
{
	struct uk_posix_task *task = NULL;

	uk_mutex_lock(&pprocess_mutex);
	if (tid > 0 && tid < TIDMAP_SIZE &&
	    process_task_attached(process, tid_identity[tid])) {
		task = tid_identity[tid];
		uk_refcount_acquire(&task->references);
	}
	uk_mutex_unlock(&pprocess_mutex);
	return task;
}

__u64 uk_posix_process_task_serial(const struct uk_posix_task *task)
{
	return task->serial;
}

void uk_posix_process_task_acquire(struct uk_posix_task *task)
{
	uk_refcount_acquire(&task->references);
}

void uk_posix_process_task_put(struct uk_posix_task *task)
{
	pprocess_task_put(task);
}

bool uk_posix_process_task_attached(struct posix_process *process,
				    struct uk_posix_task *task)
{
	bool attached;

	uk_mutex_lock(&pprocess_mutex);
	attached = process_task_attached(process, task);
	uk_mutex_unlock(&pprocess_mutex);
	return attached;
}

int uk_posix_process_task_name(struct posix_process *process,
			       struct uk_posix_task *identity,
			       char name[UK_POSIX_TASK_NAME_SIZE])
{
	char *current_name;
	int rc = -ESRCH;

	uk_mutex_lock(&pprocess_mutex);
	current_name = process_task_name_buffer(process, identity);
	if (current_name) {
		memcpy(name, current_name, UK_POSIX_TASK_NAME_SIZE);
		rc = 0;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

int uk_posix_process_task_status(struct posix_process *process,
				 struct uk_posix_task *identity,
				 struct uk_posix_task_status *status)
{
	struct posix_thread *thread;
	char *name;
	int rc = -ESRCH;

	*status = (struct uk_posix_task_status){0};
	uk_mutex_lock(&pprocess_mutex);
	name = process_task_name_buffer(process, identity);
	if (!name)
		goto out;
	memcpy(status->name, name, sizeof(status->name));
	status->pid = identity->number;
	status->tgid = process->pid;
	status->ppid = process->parent ? process->parent->pid : 0;
	status->threads = 0;
	uk_list_for_each_entry(thread, &process->threads, thread_list_entry)
		status->threads++;
	if (!tid_thread[process->pid] && tid_identity[process->pid] &&
	    tid_identity[process->pid]->retired)
		status->threads++;
	thread = tid2pthread(identity->number);
#if CONFIG_LIBPOSIX_USER
	if (thread) {
		status->credentials =
			pu_credentials_acquire(thread->thread,
					       &status->no_new_privs);
	} else if (identity->credentials) {
		status->credentials =
		    pu_credentials_retain(identity->credentials);
		status->no_new_privs = identity->no_new_privs;
	}
	if (!status->credentials)
		goto out;
#endif
	if (!thread || thread->state >= POSIX_THREAD_EXITED)
		status->state = UK_POSIX_TASK_ZOMBIE;
	else if (uk_thread_is_runnable(thread->thread))
		status->state = UK_POSIX_TASK_RUNNABLE;
	else
		status->state = UK_POSIX_TASK_BLOCKED;
	rc = 0;
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

void uk_posix_process_task_status_release(struct uk_posix_task_status *status)
{
#if CONFIG_LIBPOSIX_USER
	if (status->credentials) {
		pu_credentials_release(status->credentials);
		status->credentials = NULL;
	}
#else
	(void)status;
#endif
}

int uk_posix_process_task_set_name(struct posix_process *process,
				   struct uk_posix_task *identity,
				   const char name[UK_POSIX_TASK_NAME_SIZE])
{
	struct posix_thread *caller;
	char *current_name;
	int rc = -ESRCH;

	uk_mutex_lock(&pprocess_mutex);
	current_name = process_task_name_buffer(process, identity);
	caller = uk_pthread_current();
	if (current_name) {
		if (!caller || caller->process != process) {
			rc = -EINVAL;
		} else {
			memcpy(current_name, name, UK_POSIX_TASK_NAME_SIZE);
			rc = 0;
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

pid_t uk_posix_process_next_pid(pid_t after)
{
	pid_t next = 0;

	if (after >= TIDMAP_SIZE - 1)
		return 0;

	uk_mutex_lock(&pprocess_mutex);
	for (pid_t pid = MAX(after, 0) + 1; pid < TIDMAP_SIZE; pid++) {
		if (pid_process[pid]) {
			next = pid;
			break;
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	return next;
}
#endif
