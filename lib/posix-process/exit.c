/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <sys/types.h>
#include <stddef.h>

#include <uk/config.h>
#include <uk/process.h>
#include <uk/sched.h>
#include <uk/syscall.h>

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#include "signal/timer.h"
#include "signal.h"
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#include "process.h"

int pprocess_exit_status = PPROCESS_EXIT_STATUS_UNSET;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

/* pprocess exit state to pthread exit state */
#define PP2PT_EXIT_STATE(_pp_state)					\
	((_pp_state) == POSIX_PROCESS_EXITED ? POSIX_THREAD_EXITED :	\
					      POSIX_THREAD_KILLED)

static void pprocess_reparent_children(struct posix_process *pprocess)
{
	struct posix_process *pchild, *pchildn;
	struct posix_process *pprocess_init;

	pprocess_init = pid2pprocess(UK_PID_INIT);

	uk_list_for_each_entry_safe(pchild, pchildn, &pprocess->children,
				    child_list_entry) {
		uk_list_del(&pchild->child_list_entry);
		if (pprocess_init && pprocess != pprocess_init) {
			pchild->parent = pprocess_init;
			uk_list_add(&pchild->child_list_entry,
				    &pprocess_init->children);
			uk_waitq_wake_up(&pprocess_init->child_wait);
			uk_pr_debug("Process PID %d re-assigned to PID %d\n",
				    pchild->pid, pchild->parent->pid);
		} else {
			/* There is no parent, disconnect */
			pchild->parent = NULL;
			uk_pr_debug("Process PID %d loses its parent\n",
				    pchild->pid);
		}
	}
}

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
static inline int signal_exit(struct posix_process *pprocess)
{
	struct kern_sigaction *ks;
	__u64 exit_signal;

	UK_ASSERT(pprocess);
	UK_ASSERT(pprocess->signal);

	if (pprocess->exit_signal)
		exit_signal = pprocess->exit_signal;
	else
		exit_signal = SIGCHLD;

	ks = KERN_SIGACTION(pprocess, exit_signal);
	if (ks->ks_handler == SIG_IGN || ks->ks_flags & SA_NOCLDWAIT)
		return 1;

	return pprocess_signal_send(pprocess, exit_signal, NULL);
}
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

/* Exit AND release pthread.
 * Notice: Unlike pprocesses, pthreads are released upon exit.
 */
void pprocess_exit_pthread(struct posix_thread *pthread,
			   enum posix_thread_state state,
			   struct pprocess_retired *retired)
{
	struct posix_process_ptexit_event_data event_data;
	struct posix_process *pprocess;

	UK_ASSERT(state == POSIX_THREAD_EXITED ||
		  state == POSIX_THREAD_KILLED);

	UK_ASSERT(pthread);
	UK_ASSERT(pthread->state != POSIX_THREAD_EXITED &&
		  pthread->state != POSIX_THREAD_KILLED);

	pprocess = pthread->process;
	UK_ASSERT(pprocess);
	UK_ASSERT(pprocess->state == POSIX_PROCESS_RUNNING);

	uk_pr_debug("pid %d: exit tid %d\n", pprocess->pid, pthread->tid);

	UK_ASSERT(pthread->thread == uk_thread_current() || !pthread->thread->sched);

	/* Update state */
	pthread->state = state;

	/* Raise event */
	event_data.thread = pthread->thread;
	event_data.tid = pthread->tid;
	pprocess_raise_ptexit_event(&event_data);

	pprocess_complete_vfork(pthread);

	pprocess_release_pthread(pthread, retired);
}

static void request_thread_exit(struct posix_thread *thread,
				enum posix_thread_state state)
{
	uk_store_n(&thread->exit_state, state);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_wake_waiter(thread);
#endif
	uk_waitq_wake_up(&thread->process->child_wait);
	uk_waitq_wake_up(&thread->process->thread_wait);
	if (uk_load_n(&thread->user_started) &&
	    thread->state != POSIX_THREAD_BLOCKED_VFORK)
		uk_thread_wake(thread->thread);
}

void pprocess_exit(struct posix_process *process,
		   enum posix_process_state state, int status)
{
	struct posix_thread *thread, *next;

	UK_ASSERT(state == POSIX_PROCESS_EXITED || state == POSIX_PROCESS_KILLED);
	uk_mutex_lock(&pprocess_mutex);
	if (process->state != POSIX_PROCESS_RUNNING) {
		uk_mutex_unlock(&pprocess_mutex);
		return;
	}
	if (process->exit_state == POSIX_PROCESS_RUNNING) {
		process->exit_state = state;
		process->exit_status = status;
	}
	uk_pprocess_foreach_pthread(process, thread, next)
		request_thread_exit(thread, PP2PT_EXIT_STATE(process->exit_state));
	uk_mutex_unlock(&pprocess_mutex);
}

int pprocess_stop_others(struct posix_process *process)
{
	struct posix_thread *self = uk_pthread_current();
	struct posix_thread *thread, *next;
	int rc = 0;

	uk_mutex_lock(&pprocess_mutex);
	if (pprocess_thread_exit_requested(self)) {
		rc = -EINTR;
		goto out;
	}
	if (process->exec_owner) {
		rc = -EAGAIN;
		goto out;
	}
	process->exec_owner = self->thread;
	uk_pprocess_foreach_pthread(process, thread, next)
		if (thread != self)
			request_thread_exit(thread, POSIX_THREAD_EXITED);
	while (!uk_list_is_singular(&process->threads)) {
		if (pprocess_thread_exit_requested(self)) {
			rc = -EINTR;
			break;
		}
		uk_waitq_wait_locked(&process->thread_wait,
			uk_mutex_lock, uk_mutex_unlock, &pprocess_mutex);
	}
	if (pprocess_thread_exit_requested(self))
		rc = -EINTR;
	process->exec_owner = NULL;
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

bool uk_posix_process_exit_requested(void)
{
	struct posix_thread *thread = uk_pthread_current();

	return thread && pprocess_thread_exit_requested(thread);
}

void pprocess_enter_application(void)
{
	struct posix_thread *thread = uk_pthread_current();
	if (!thread)
		return;
	uk_store_n(&thread->user_started, true);
	if (pprocess_thread_exit_requested(thread)) {
		uk_lcpu_enable_irq();
		uk_sched_thread_exit();
	}
}

static void process_syscall_enter(struct uk_syscall_enter_ctx *ctx)
{
	if (ctx->execenv && (ctx->flags & UK_SYSCALL_ENTER_CTX_BINARY_SYSCALL))
		pprocess_enter_application();
}

static void process_syscall_exit(struct uk_syscall_exit_ctx *ctx)
{
	if (ctx->execenv && (ctx->flags & UK_SYSCALL_EXIT_CTX_BINARY_SYSCALL))
		pprocess_enter_application();
}

uk_syscall_entertab_prio(process_syscall_enter, UK_PRIO_EARLIEST);
uk_syscall_exittab_prio(process_syscall_exit, UK_PRIO_EARLIEST);

void pprocess_finish_exit(struct posix_process *pprocess,
			  struct posix_thread *thread,
			  struct pprocess_retired *retired)
{
	struct posix_process_ppexit_event_data event_data;
	struct posix_process *parent_process;
	enum posix_process_state state = pprocess->exit_state;
	int exit_status = pprocess->exit_status;
	__bool nowait = false;
	int ret __maybe_unused;

	UK_ASSERT(uk_list_is_singular(&pprocess->threads));
	if (state == POSIX_PROCESS_RUNNING) {
		state = thread->exit_state == POSIX_THREAD_KILLED ?
			POSIX_PROCESS_KILLED : POSIX_PROCESS_EXITED;
		exit_status = thread->exit_status;
	}

	uk_pr_debug("pid %d: exit process\n", pprocess->pid);

	/* May be NULL if init terminated early */
	parent_process = pprocess->parent;

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	/* As this will be signaling the parent on behalf of the current
	 * pthread, we must do this before we terminate the pthreads, in
	 * case we are called from _exit(), exit_group() (i.e. operate on
	 * current).
	 */
	if (parent_process) {
		ret = signal_exit(parent_process);
		if (ret > 0) {
			/* From exit(2): "If the parent has set SA_NOCLDWAIT, or
			 * has set the SIGCHLD handler to SIG_IGN, the status is
			 * discarded and the child dies immediately."
			 */
			uk_pr_info("Parent ignores SIGHLD, terminating\n");
			nowait = true;
		} else if (unlikely(ret < 0)) { /* no choice here but crash */
			UK_CRASH("Could not signal parent (%d)\n", ret);
		}
	}
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

	pprocess_exit_pthread(thread, PP2PT_EXIT_STATE(state), retired);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_timers_stop_locked(pprocess, true, retired);
#endif

	UK_ASSERT(uk_list_empty(&pprocess->threads));

	parent_process = pprocess->parent;

	/* Update process state & exit_status*/
	if (state == POSIX_PROCESS_EXITED)
		pprocess->exit_status = exit_status & 0xff;
	else /* POSIX_PROCESS_KILLED */
		pprocess->exit_status = exit_status;

	/* Reparent child processes to init */
	pprocess_reparent_children(pprocess);

	/* Notify handlers */
	event_data.pid = pprocess->pid;
	pprocess_vm_retire_locked(pprocess->vm_context, pprocess->pid, retired);
	pprocess->vm_context = NULL;
	pprocess_raise_ppexit_event(&event_data);

	pprocess->state = state;
	uk_semaphore_up(&pprocess->exit_semaphore);

	if (nowait)
		pprocess_release(pprocess, retired);
	else if (parent_process)
		uk_waitq_wake_up(&parent_process->child_wait);
}

/* NOTE: The man pages of _exit(2) say:
 *       "In glibc up to version 2.3, the _exit() wrapper function invoked
 *        the kernel system call of the same name.  Since glibc 2.3, the
 *        wrapper function invokes exit_group(2), in order to terminate all
 *        of the threads in a process.
 *        The raw _exit() system call terminates only the calling thread,
 *        and actions such as reparenting child processes or sending
 *        SIGCHLD to the parent process are performed only if this is the
 *        last thread in the thread group."
 */
UK_LLSYSCALL_R_DEFINE(int, exit, int, status)
{
	struct posix_thread *thread = uk_pthread_current();

	uk_mutex_lock(&pprocess_mutex);
	if (!pprocess_thread_exit_requested(thread)) {
		thread->exit_status = status;
		uk_store_n(&thread->exit_state, POSIX_THREAD_EXITED);
	}
	uk_mutex_unlock(&pprocess_mutex);
	uk_sched_thread_exit();
}

UK_LLSYSCALL_R_DEFINE(int, exit_group, int, status)
{
	struct posix_process *pprocess;

	pprocess = uk_pprocess_current();
	UK_ASSERT(pprocess);

	pprocess_exit(pprocess, POSIX_PROCESS_EXITED, status);
	uk_sched_thread_exit();
}
#else /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
__noreturn void pprocess_exit_stub(int status)
{
	if (pprocess_thread_main) {
		pprocess_exit_status = status;
		uk_sched_thread_exit(); /* noreturn */
	}

	UK_CRASH("Invalid system configuration:\n"
		 "Process support without multithreading does not support _exit() / exit_group()\n"
		 "Select LIBPOSIX_PROCESS_MULTITHREADING, or at minimum LIBUKBOOT_MAINTHREAD\n");
}

UK_LLSYSCALL_R_DEFINE(int, exit, int, status)
{
	pprocess_exit_stub(status);
	UK_BUG(); /* noreturn */
}

UK_LLSYSCALL_R_DEFINE(int, exit_group, int, status)
{
	pprocess_exit_stub(status);
	UK_BUG(); /* noreturn */
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#if UK_LIBC_SYSCALLS
__noreturn void exit(int status)
{
	/* According to _exit(2): "Since glibc 2.3, the wrapper function invokes
	 * exit_group(2), in order to terminate all of the threads in a
	 * process."
	 */
	uk_syscall_e_exit_group(status);
	UK_BUG(); /* noreturn */
}

__noreturn void exit_group(int status)
{
	uk_syscall_e_exit_group(status);
	UK_BUG(); /* noreturn */
}
#endif /* UK_LIBC_SYSCALLS */
