/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>

#include <uk/arch/ctx.h>
#include <uk/essentials.h>
#include <uk/plat/config.h>
#include <uk/prio.h>
#include <uk/process.h>
#include <uk/syscall.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif

#include "process.h"
#include "../rseq.h"
#include "sigset.h"
#include "signal.h"
#include "siginfo.h"

static void __noreturn uk_sigact_term(int sig)
{
	struct posix_process *pprocess;

	pprocess = uk_pprocess_current();
	UK_ASSERT(pprocess);

	uk_pr_info("pid: %d terminated by signal\n", pprocess->pid);

	pprocess_exit(pprocess, POSIX_PROCESS_KILLED, sig);
	uk_sched_thread_exit(); /* noreturn */
}

static void uk_sigact_ign(int __unused sig)
{
	UK_BUG(); /* We should never reach this point */
}

static void uk_sigact_core(int __unused sig)
{
	uk_pr_warn("%d: SIG_CORE not supported, falling back to SIG_TERM\n",
		   sig);
	uk_sigact_term(sig);
}

static void uk_sigact_stop(int __unused sig)
{
	uk_pr_warn("SIG_STOP not supported\n");
}

static void uk_sigact_cont(int __unused sig)
{
	uk_pr_warn("SIG_CONT not supported\n");
}

bool pprocess_signal_is_deliverable(struct posix_thread *pthread, int signum)
{
	struct posix_process *proc;

	UK_ASSERT(pthread);
	UK_ASSERT(signum);

	proc = tid2pprocess(pthread->tid);
	UK_ASSERT(proc);

	return (!IS_MASKED(pthread, signum) && !IS_IGNORED(proc, signum));
}

/* Deliver a signal to a thread. The caller must check that the signal
 * is not masked by the handling thread or ignored by the process or its
 * default disposition.
 */
static bool do_deliver(struct posix_thread *pthread, struct uk_signal *sig,
		       struct ukarch_execenv *execenv,
		       const uk_sigset_t *restore_mask)
{
	struct posix_process *pproc;
	uk_sigset_t saved_mask;
	struct kern_sigaction action;
	bool prepared = false;
	int signum;

	UK_ASSERT(sig && sig->siginfo.si_signo);
	UK_ASSERT(pthread);

	UK_ASSERT(pthread == uk_pthread_current());
	pproc = pthread->process;
	UK_ASSERT(pproc);

	signum = sig->siginfo.si_signo;
	uk_mutex_lock(&pprocess_mutex);
	action = *KERN_SIGACTION(pproc, signum);
	saved_mask = restore_mask ? *restore_mask : pthread->signal->mask;
	if (action.ks_handler == SIG_IGN)
		goto out_restore_mask;
	if (action.ks_handler != SIG_DFL) {
		uk_sigorset(&pthread->signal->mask, &action.ks_mask);
		if (!(action.ks_flags & SA_NODEFER))
			SET_MASKED(pthread, signum);
		if (action.ks_flags & SA_RESETHAND)
			pprocess_signal_sigaction_clear(KERN_SIGACTION(pproc, signum));
	}
	uk_mutex_unlock(&pprocess_mutex);

	/* Execute the default action if a signal handler is not defined */
	if (action.ks_handler == SIG_DFL) {
		/* Standard signals: Invoke signal-specific default action */
		if (signum < UK_SIGNAL_RTMIN) {
			if (UK_BIT(signum) & SIGACT_CORE_MASK)
				uk_sigact_core(signum);
			else if (UK_BIT(signum) & SIGACT_TERM_MASK)
				uk_sigact_term(signum);
			else if (UK_BIT(signum) & SIGACT_STOP_MASK)
				uk_sigact_stop(signum);
			else if (UK_BIT(signum) & SIGACT_CONT_MASK)
				uk_sigact_cont(signum);
			else if (UK_BIT(signum) & SIGACT_IGN_MASK)
				uk_sigact_ign(signum);
			else
				UK_BUG();
		} else {
			/* Real-time signals: SIG_TERM by default */
			uk_sigact_term(signum);
		}
		uk_mutex_lock(&pprocess_mutex);
		goto out_restore_mask;
	}

	pprocess_rseq_interrupt(execenv);
	pprocess_signal_arch_prepare(sig, &action, execenv, &saved_mask);
	prepared = true;

	uk_mutex_lock(&pprocess_mutex);
out_restore_mask:
	if (!prepared)
		pthread->signal->mask = saved_mask;
	if ((pthread->signal->sigqueue.pending | pproc->signal->sigqueue.pending) &
	    ~pthread->signal->mask)
		uk_store_n(&pthread->signal->notification, true);
	uk_mutex_unlock(&pprocess_mutex);
	return prepared;
}

static bool deliver_pending(struct posix_thread *thread, bool process_queue,
			    struct ukarch_execenv *execenv)
{
	struct posix_process *proc = thread->process;
	struct posix_thread *target = process_queue ? NULL : thread;
	struct uk_alloc *alloc = process_queue ? proc->_a : thread->_a;
	struct uk_signal *sig;
	struct uk_signal delivery;
	int signum;

	pprocess_signal_foreach(signum) {
		for (;;) {
			uk_mutex_lock(&pprocess_mutex);
			sig = pprocess_signal_is_deliverable(thread, signum) ?
				pprocess_signal_dequeue(proc, target, signum) : NULL;
			uk_mutex_unlock(&pprocess_mutex);
			if (!sig)
				break;
			delivery.siginfo = sig->siginfo;
			uk_signal_free(alloc, sig);
			if (do_deliver(thread, &delivery, execenv, NULL))
				return true;
		}
	}
	return false;
}

static bool deliver_waited(struct posix_thread *thread,
			   struct ukarch_execenv *execenv)
{
	struct uk_signal_tdesc *td = thread->signal;
	struct uk_signal sig;
	uk_sigset_t saved_mask;
	bool restore_mask;

	uk_mutex_lock(&pprocess_mutex);
	if (!td->wait_pending) {
		uk_mutex_unlock(&pprocess_mutex);
		return false;
	}
	sig.siginfo = td->wait_info;
	restore_mask = td->restore_mask;
	saved_mask = td->saved_mask;
	td->wait_pending = false;
	td->restore_mask = false;
	uk_mutex_unlock(&pprocess_mutex);
	return do_deliver(thread, &sig, execenv, restore_mask ? &saved_mask : NULL);
}

void pprocess_signal_deliver(struct ukarch_execenv *execenv)
{
	struct posix_thread *pthread;
	struct posix_process *pproc;

	/* FIXME: This can go away once we eliminate the last syscall made
	 *        by kernel context and pass execenv to syscalls in native
	 *        mode.
	 */
	if (!execenv)
		return;

	pthread = uk_pthread_current();
	UK_ASSERT(pthread);
	uk_store_n(&pthread->signal->notification, false);

	pproc = uk_pprocess_current();
	UK_ASSERT(pproc);

	/* SIGSTOP / SIGCONT should have been already ignored by rt_sigaction */
	UK_ASSERT(!IS_PENDING(pproc->signal->sigqueue, SIGSTOP));
	UK_ASSERT(!IS_PENDING(pproc->signal->sigqueue, SIGCONT));

	if (deliver_waited(pthread, execenv))
		return;

	/* Deliver all pending signals of this process & thread */
	if (deliver_pending(pthread, true, execenv))
		return;
	if (pthread->state == POSIX_THREAD_RUNNING ||
	    pthread->state == POSIX_THREAD_BLOCKED_SIGNAL)
		deliver_pending(pthread, false, execenv);
}

static void uk_signal_deliver(struct uk_syscall_exit_ctx *exit_ctx)
{
	pprocess_signal_deliver(exit_ctx->execenv);
}

uk_syscall_exittab_prio(uk_signal_deliver, UK_PRIO_BEFORE(UK_PRIO_LATEST));

/* We land here from the trap handler that executes in exception context.
 * Once we return, the trampoline will pass control back to the application.
 */
void sys_error_handler(struct ukarch_execenv *ee __unused, long arg)
{
	struct ukarch_auxspcb *auxspcb;
	struct sys_error_desc *error;
	struct posix_thread *pthread;
	struct uk_signal sig = {0};

	/* Now we can enable IRQs */
	uk_lcpu_enable_irq();

	/* Get arg */
	error = (struct sys_error_desc *)arg;
	UK_ASSERT(error);

	/* Switch to uk sysregs */
	auxspcb = ukarch_auxsp_get_cb(error->auxsp);
	uk_lcpu_sysctx_load((struct uk_lcpu_sysctx *)auxspcb->uksysctx);

	pthread = uk_pthread_current();
	UK_ASSERT(pthread);

	pprocess_enter_application();

#if CONFIG_LIBUKVMEM && CONFIG_LIBPOSIX_PROCESS_EXECVE
	if (error->signum == SIGSEGV && !error->code &&
	    !pprocess_memory_stack(error->vaddr))
		pprocess_execenv_load((long)ee);
#endif

	if (!pprocess_signal_is_deliverable(pthread, error->signum))
		uk_sigact_term(error->signum);

	/* Prepare siginfo */
	sig.siginfo.si_signo = error->signum;
	sig.siginfo.si_code = error->code ? error->code : SI_KERNEL;
	sig.siginfo.si_addr = (void *)error->vaddr;
	if (!error->code && error->signum == SIGSEGV) {
		sig.siginfo.si_code = SEGV_MAPERR;
#if CONFIG_LIBUKVMEM
		struct uk_vas *vas = uk_vas_get_active();
		const struct uk_vma *vma;

		uk_mutex_lock(&vas->mutation_lock);
		vma = uk_vma_find(vas, error->vaddr);
		if (vma && vma->ops == &uk_vma_stack_ops) {
			__vaddr_t start = vma->start + UK_VMA_STACK_BOTTOM_GUARD_SIZE;
			__vaddr_t end = vma->end - UK_VMA_STACK_TOP_GUARD_SIZE;

			if (!IN_RANGE(error->vaddr, start, end - start))
				vma = NULL;
		}
		if (vma)
			sig.siginfo.si_code = SEGV_ACCERR;
		uk_mutex_unlock(&vas->mutation_lock);
#endif
	}

	/* Execute standard delivery path */
	do_deliver(pthread, &sig, ee, NULL);
}

void pprocess_signal_restore_mask(uk_sigset_t mask)
{
	struct posix_thread *thread = uk_pthread_current();
	struct posix_process *process = thread->process;

	uk_sigdelset(&mask, SIGKILL);
	uk_sigdelset(&mask, SIGSTOP);
	uk_mutex_lock(&pprocess_mutex);
	thread->signal->mask = mask;
	if ((thread->signal->sigqueue.pending |
	     process->signal->sigqueue.pending) & ~mask)
		uk_store_n(&thread->signal->notification, true);
	uk_mutex_unlock(&pprocess_mutex);
}
