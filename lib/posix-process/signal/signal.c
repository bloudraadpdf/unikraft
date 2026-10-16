/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/process.h>
#include <uk/refcount.h>
#include <string.h>
#include <uk/plat/time.h>
#include <uk/posix-signal-wait.h>

#include "process.h"
#include "sigset.h"
#include "siginfo.h"
#include "signal.h"

/* Check permissions to signal target process.
 *
 * For a process to be permitted to send a signal to another
 * process, its real / effective uid must match the target's
 * real / saved set-user-id. We don't support these, so we
 * deliver to every process.
 */
#define signal_check_perm(_target_process) true

void pprocess_signal_wake_waiter(struct posix_thread *thread)
{
	struct uk_posix_signal_wait *wait = thread->signal->poll_wait;

	if (wait) {
		uk_store_n(&wait->interrupted, 1);
		if (wait->queue)
			uk_waitq_wake_up(wait->queue);
		else
			uk_thread_wake(thread->thread);
	} else {
		uk_waitq_wake_up(&thread->signal->waitq);
	}
}

static void signal_notify_thread(struct posix_thread *thread)
{
	if (thread->state == POSIX_THREAD_BLOCKED_SIGNAL)
		pprocess_signal_wake_waiter(thread);
	pprocess_signal_notify(thread);
}

/* Enqueue signal for a thread or a process
 *
 * Setting pthread to NULL signifies that the signal
 * targets the process.
 */
static int pprocess_signal_enqueue(struct posix_process *pproc,
				   struct posix_thread *pthread,
				   struct uk_signal *sig)
{
	struct uk_signal_queue *queue;
	struct posix_thread *thread, *next;
	int signum = sig->siginfo.si_signo;
	int rc = 0;

	UK_ASSERT(!pthread || pthread->process == pproc);
	queue = pthread ? &pthread->signal->sigqueue : &pproc->signal->sigqueue;
	uk_mutex_lock(&pprocess_mutex);
	if (signum < UK_SIGNAL_RTMIN && IS_PENDING(*queue, signum)) {
		uk_signal_free(pthread ? pthread->_a : pproc->_a, sig);
	} else if (pproc->signal->queued_count >= pproc->signal->queued_max) {
		rc = -EAGAIN;
	} else {
		uk_list_add_tail(&sig->list_head, &queue->list_head[signum]);
		SET_PENDING(*queue, signum);
		pproc->signal->queued_count++;
	}
	if (!rc) {
		if (pthread) {
			signal_notify_thread(pthread);
		} else {
			uk_pprocess_foreach_pthread(pproc, thread, next)
				signal_notify_thread(thread);
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static struct uk_signal *signal_dequeue_locked(struct posix_process *pproc,
					  struct posix_thread *pthread,
					  int signum)
{
	struct uk_signal_queue *sigqueue;
	struct uk_signal *sig;

	UK_ASSERT(pproc);

	if (pthread) {
		UK_ASSERT(pthread->process == pproc);
		sigqueue = &pthread->signal->sigqueue;
	} else {
		sigqueue = &pproc->signal->sigqueue;
	}

	sig = uk_list_first_entry_or_null(&sigqueue->list_head[signum],
					  struct uk_signal,
					  list_head);
	if (!sig)
		return sig;

	uk_list_del(&sig->list_head);

	/* Reset pending bit if the signal queue is empty */
	if (uk_list_empty(&sigqueue->list_head[signum])) {
		if (pthread)
			RESET_PENDING(pthread->signal->sigqueue, signum);
		else
			RESET_PENDING(pproc->signal->sigqueue, signum);
	}

	pproc->signal->queued_count--;

	return sig;
}

struct uk_signal *pprocess_signal_dequeue(struct posix_process *pproc,
					  struct posix_thread *pthread,
					  int signum)
{
	struct uk_signal *sig;

	uk_mutex_lock(&pprocess_mutex);
	sig = signal_dequeue_locked(pproc, pthread, signum);
	uk_mutex_unlock(&pprocess_mutex);
	return sig;
}

int pprocess_signal_take_locked(struct posix_thread *thread,
			      const uk_sigset_t *set, siginfo_t *info)
{
	struct posix_process *proc = thread->process;
	struct posix_thread *target;
	struct uk_signal *sig;
	int signum;

	for (int queue = 0; queue < 2; queue++) {
		target = queue ? NULL : thread;
		pprocess_signal_foreach(signum) {
			if (set ? !uk_sigismember(set, signum) :
			    !pprocess_signal_is_deliverable(thread, signum))
				continue;
			sig = signal_dequeue_locked(proc, target, signum);
			if (!sig)
				continue;
			if (info)
				*info = sig->siginfo;
			uk_signal_free(target ? thread->_a : proc->_a, sig);
			return signum;
		}
	}
	return 0;
}

int uk_posix_signal_wait_init(struct uk_posix_signal_wait *wait,
			      struct uk_waitq *queue,
			      const sigset_t *mask, size_t size)
{
	struct posix_thread *thread = uk_pthread_current();
	struct uk_signal_tdesc *td;

	if (mask && size != sizeof(uk_sigset_t))
		return -EINVAL;
	*wait = (struct uk_posix_signal_wait){
		.thread = thread, .queue = queue, .masked = mask != NULL
	};
	if (!thread)
		return mask ? -ENOSYS : 0;
	td = thread->signal;
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(!td->poll_wait);
	if (mask) {
		wait->saved_mask = td->mask;
		memcpy(&td->mask, mask, sizeof(td->mask));
		uk_sigdelset(&td->mask, SIGKILL);
		uk_sigdelset(&td->mask, SIGSTOP);
	}
	td->poll_wait = wait;
	thread->state = POSIX_THREAD_BLOCKED_SIGNAL;
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
}

int uk_posix_signal_wait_check(struct uk_posix_signal_wait *wait)
{
	struct uk_signal_tdesc *td;
	int rc = 0;

	if (!wait->thread)
		return 0;
	td = wait->thread->signal;
	uk_mutex_lock(&pprocess_mutex);
	uk_store_n(&wait->interrupted, 0);
	if (pprocess_thread_exit_requested(wait->thread)) {
		rc = -EINTR;
	} else if (pprocess_signal_take_locked(wait->thread, NULL, &td->wait_info)) {
		td->wait_pending = true;
		rc = -EINTR;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

void uk_posix_signal_wait_end(struct uk_posix_signal_wait *wait, int result)
{
	struct uk_signal_tdesc *td;

	if (!wait->thread)
		return;
	td = wait->thread->signal;
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(td->poll_wait == wait);
	td->poll_wait = NULL;
	if (wait->masked) {
		if (result == -EINTR) {
			td->saved_mask = wait->saved_mask;
			td->restore_mask = true;
		} else {
			td->mask = wait->saved_mask;
		}
	}
	wait->thread->state = POSIX_THREAD_RUNNING;
	uk_mutex_unlock(&pprocess_mutex);
}

int pprocess_signal_wait_locked(struct posix_thread *thread,
			       const uk_sigset_t *set, siginfo_t *info,
			       __nsec deadline)
{
	struct uk_signal_tdesc *td = thread->signal;
	int rc;

	UK_ASSERT(!td->wait_pending);
	for (;;) {
		if (pprocess_thread_exit_requested(thread)) {
			rc = -EINTR;
			break;
		}
		if (set && (rc = pprocess_signal_take_locked(thread, set, info)))
			break;
		if (pprocess_signal_take_locked(thread, NULL, &td->wait_info)) {
			td->wait_pending = true;
			rc = -EINTR;
			break;
		}
		if (deadline && ukplat_monotonic_clock() >= deadline) {
			rc = -EAGAIN;
			break;
		}
		thread->state = POSIX_THREAD_BLOCKED_SIGNAL;
		uk_waitq_wait_deadline_locked(&td->waitq, deadline,
			uk_mutex_lock, uk_mutex_unlock, &pprocess_mutex);
	}
	thread->state = POSIX_THREAD_RUNNING;
	return rc;
}

void pprocess_signal_clear_pending(struct posix_process *proc, int signum)
{
	struct posix_thread *thread, *threadn;
	struct uk_signal *sig;

	if (IS_PENDING(proc->signal->sigqueue, signum)) {
		while ((sig = pprocess_signal_dequeue(proc, NULL, signum)))
			uk_signal_free(proc->_a, sig);
	}

	uk_pprocess_foreach_pthread(proc, thread, threadn) {
		if (!IS_PENDING(thread->signal->sigqueue, signum))
			continue;
		while ((sig = pprocess_signal_dequeue(proc, thread, signum)))
			uk_signal_free(thread->_a, sig);
	}
}

void pprocess_signal_sigaction_clear(struct kern_sigaction *ks)
{
	ks->ks_handler = SIG_DFL;
	ks->ks_flags = 0;
	ks->ks_restorer = NULL;
	uk_sigemptyset(&ks->ks_mask);
}

struct uk_sigaction *pprocess_signal_sigaction_alloc(struct uk_alloc *alloc)
{
	struct uk_sigaction *actions = uk_malloc(alloc, sizeof(*actions));

	if (unlikely(!actions)) {
		uk_pr_err("Could not allocate memory\n");
		return NULL;
	}
	actions->alloc = alloc;
	uk_refcount_init(&actions->refcnt, 1);
	return actions;
}

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
void pprocess_signal_exec(struct uk_sigaction *actions)
{
	struct posix_thread *thread = uk_pthread_current();
	struct posix_process *process = thread->process;
	struct uk_sigaction *previous;
	int signum;

	uk_mutex_lock(&pprocess_mutex);
	previous = process->signal->sigaction;
	pprocess_signal_foreach(signum) {
		pprocess_signal_sigaction_clear(&actions->ks[signum]);
		if (previous->ks[signum].ks_handler == SIG_IGN)
			actions->ks[signum].ks_handler = SIG_IGN;
	}
	process->signal->sigaction = actions;
	thread->signal->altstack = (stack_t){ .ss_flags = SS_DISABLE };
	pprocess_signal_sigaction_release(previous);
	uk_mutex_unlock(&pprocess_mutex);
}
#endif

int pprocess_signal_send(struct posix_process *proc, int signum,
			 siginfo_t *siginfo)
{
	struct uk_signal *sig;
	int rc;

	if (signum == SIGKILL) {
		pprocess_exit(proc, POSIX_PROCESS_KILLED, signum);
		return 0;
	}

	sig = uk_signal_alloc(proc->_a);
	if (unlikely(!sig)) {
		uk_pr_err("Could not allocate signal\n");
		return -EAGAIN;
	}

	if (siginfo)
		set_siginfo_sigqueue(signum, &sig->siginfo, siginfo);
	else
		set_siginfo_kill(signum, &sig->siginfo);

	rc = pprocess_signal_enqueue(proc, NULL, sig);
	if (unlikely(rc)) {
		/* issue a warning as this may be temporary */
		uk_pr_warn("Could not queue signal\n");
		uk_signal_free(proc->_a, sig);
		return -EAGAIN;
	}

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	pprocess_signal_files_notify(proc, signum);
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */

	return 0;
}

static int signal_process_locked(pid_t pid, int signum, siginfo_t *siginfo)
{
	struct posix_process *pproc = NULL;
	int rc;

	if (unlikely(signum != 0 && !IS_VALID(signum)))
		return -EINVAL;

	if (unlikely(signum == SIGSTOP || signum == SIGCONT)) {
		uk_pr_warn("Process stop / resume not supported. Ignoring\n");
		return 0;
	}

	/* pid == 0 -> Send the signal to every process in the process group
	 *             of the calling process.
	 * pid < -1 -> Send the signal to every process in the process group
	 *             with ID equal to -pid.
	 */
	if (!pid || pid < -1) {
		uk_pr_warn("pgroups not supported, delivering to every process\n");
		pid = -1;
	}

	/* pid == -1 -> Send the signal to every process for which the
	 *              calling process has permissions to signal.
	 */
	if (pid == -1) {
		uk_pprocess_foreach(pproc) {
			if (!signal_check_perm(pproc))
				continue;

			if (pprocess_signal_should_drop(pproc, signum))
				continue;

			rc = pprocess_signal_send(pproc, signum, siginfo);
			if (unlikely(rc))
				return rc;
		}
		return 0;
	}

	/* Default: Send to process with pid */
	pproc = pid2pprocess(pid);
	if (unlikely(!pproc))
		return -ESRCH;

	if (unlikely(signal_check_perm(pproc) == false))
		return -EPERM;

	/* If signum == 0 do only pid exist & permissions check */
	if (!signum)
		return 0;

	/* If this signal is currently ignored, don't even try */
	if (pprocess_signal_should_drop(pproc, signum))
		return 0;

	rc = pprocess_signal_send(pproc, signum, siginfo);
	if (unlikely(rc))
		return rc;

	return 0;
}

int pprocess_signal_process_do(pid_t pid, int signum, siginfo_t *siginfo)
{
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	rc = signal_process_locked(pid, signum, siginfo);
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static int signal_thread_locked(int tgid, int tid, int signum,
				siginfo_t *siginfo)
{
	struct posix_thread *pthread;
	struct posix_process *pproc;
	struct uk_signal *sig;
	int rc;

	if (signum && !IS_VALID(signum))
		return -EINVAL;
	pthread = tid2pthread(tid);
	if (unlikely(!pthread))
		return -ESRCH;

	pproc = pthread->process;
	if (tgid && pproc->pid != tgid)
		return -ESRCH;
	if (!signum)
		return 0;

	if (signum == SIGKILL)
		return pprocess_signal_send(pproc, signum, siginfo);

	sig = uk_signal_alloc(pthread->_a);
	if (unlikely(!sig)) {
		uk_pr_err("Could not allocate signal\n");
		return -EAGAIN;
	}

	if (siginfo)
		set_siginfo_sigqueue(signum, &sig->siginfo, siginfo);
	else
		set_siginfo_kill(signum, &sig->siginfo);

	rc = pprocess_signal_enqueue(pproc, pthread, sig);
	if (unlikely(rc)) {
		/* issue a warning as this may be temporary */
		uk_pr_warn("Could not queue signal\n");
		uk_signal_free(pthread->_a, sig);
		return rc;
	}

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	pthread_signal_files_notify(pproc, pthread, signum);
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */

	return 0;
}

int pprocess_signal_thread_do(int tgid, int tid, int signum, siginfo_t *siginfo)
{
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	rc = signal_thread_locked(tgid, tid, signum, siginfo);
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

int pprocess_signal_pdesc_alloc(struct posix_process *process)
{
	UK_ASSERT(process);

	process->signal = uk_malloc(process->_a,
				    sizeof(struct uk_signal_pdesc));
	if (unlikely(!process->signal)) {
		uk_pr_err("Could not allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}

int pprocess_signal_tdesc_alloc(struct posix_thread *thread)
{
	UK_ASSERT(thread);

	thread->signal = uk_malloc(thread->_a, sizeof(struct uk_signal_tdesc));
	if (unlikely(!thread->signal)) {
		uk_pr_err("Could not allocate memory\n");
		return -ENOMEM;
	}

	return 0;
}

int pprocess_signal_pdesc_init(struct posix_process *process)
{
	struct uk_signal_pdesc *pd;
	int signum;

	UK_ASSERT(process);
	UK_ASSERT(process->signal);

	pd = process->signal;

	pd->queued_count = 0;
	pd->queued_max = _POSIX_SIGQUEUE_MAX;

	uk_sigemptyset(&pd->sigqueue.pending);

	pprocess_signal_foreach(signum)
		UK_INIT_LIST_HEAD(&pd->sigqueue.list_head[signum]);

	pd->sigaction = pprocess_signal_sigaction_alloc(process->_a);
	if (unlikely(!pd->sigaction))
		return -ENOMEM;

	pprocess_signal_foreach(signum)
		pprocess_signal_sigaction_clear(KERN_SIGACTION(process, signum));

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	uk_signal_files_ctx_init(&pd->sigfiles_ctx);
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */

	return 0;
}

int pprocess_signal_tdesc_init(struct posix_thread *thread)
{
	struct uk_signal_tdesc *td;
	int signum;

	UK_ASSERT(thread);
	UK_ASSERT(thread->signal);

	td = thread->signal;

	td->altstack = (stack_t){ .ss_flags = SS_DISABLE };
	td->notification = false;
	td->wait_pending = false;
	td->restore_mask = false;
	td->saved_mask = 0;
	td->poll_wait = NULL;
	uk_sigemptyset(&td->mask);
	uk_sigemptyset(&td->sigqueue.pending);

	pprocess_signal_foreach(signum)
		UK_INIT_LIST_HEAD(&td->sigqueue.list_head[signum]);

	uk_waitq_init(&td->waitq);

	return 0;
}

void pprocess_signal_pdesc_free(struct posix_process *process)
{
	struct uk_signal *sig;
	int signum;

	UK_ASSERT(process);
	UK_ASSERT(process->signal);

	pprocess_signal_foreach(signum)
		while ((sig = pprocess_signal_dequeue(process, NULL, signum)))
			uk_signal_free(process->_a, sig);

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	uk_mutex_lock(&pprocess_mutex);
	uk_signal_files_ctx_clear(&process->signal->sigfiles_ctx);
	uk_mutex_unlock(&pprocess_mutex);
#endif
	pprocess_signal_sigaction_release(process->signal->sigaction);

	uk_free(process->_a, process->signal);
}

void pprocess_signal_tdesc_free(struct posix_thread *thread)
{
	struct posix_process *pproc;
	struct uk_signal *sig;
	int signum;

	UK_ASSERT(thread);
	UK_ASSERT(thread->signal);

	pproc = tid2pprocess(thread->tid);
	UK_ASSERT(pproc);

	pprocess_signal_foreach(signum)
		while ((sig = pprocess_signal_dequeue(pproc, thread, signum)))
			uk_signal_free(thread->_a, sig);

	uk_signal_free(thread->process->_a, thread->signal);
}
