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

#include "process.h"
#include "sigset.h"
#include "signal.h"
#include "siginfo.h"

UK_CTASSERT(__offsetof(struct pprocess_signal_handler_ctx, handler_fn) ==
	    PPROCESS_SIGHNDL_CTX_FN_OFFS);
UK_CTASSERT(__offsetof(struct pprocess_signal_handler_ctx, handler_sp) ==
	    PPROCESS_SIGHNDL_CTX_SP_OFFS);
UK_CTASSERT(__offsetof(struct pprocess_signal_handler_ctx, signo) ==
	    PPROCESS_SIGHNDL_CTX_SN_OFFS);
UK_CTASSERT(__offsetof(struct pprocess_signal_handler_ctx, siginfo) ==
	    PPROCESS_SIGHNDL_CTX_SI_OFFS);
UK_CTASSERT(__offsetof(struct pprocess_signal_handler_ctx, ucontext) ==
	    PPROCESS_SIGHNDL_CTX_UC_OFFS);

/* Notice: We implement this in pure asm, as there is a number of issues
 *         with correctly preserving context in inline asm.
 */
void pprocess_signal_jmp_handler(struct pprocess_signal_handler_ctx *h,
				 struct ukarch_execenv *execenv);

static void uk_sigact_term(int __unused sig)
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

static void handle_self(struct uk_signal *sig, const struct kern_sigaction *ks,
			struct ukarch_execenv *execenv, uk_sigset_t *saved_mask)
{
	struct pprocess_signal_handler_ctx handler_ctx;
	ucontext_t ucontext = {0};
	struct posix_thread *this_thread;
	stack_t saved_stack;
	bool switched_stack;
	stack_t *altstack;
	__uptr ulsp;

	UK_ASSERT(sig);
	UK_ASSERT(ks);
	UK_ASSERT(execenv);

	/* We expect to be operating on the aux stack */
	UK_ASSERT(SP_IN_AUXSP(uk_arch_read_sp(),
			      uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM)));

	this_thread = uk_pthread_current();
	UK_ASSERT(this_thread);

	altstack = &this_thread->signal->altstack;
	saved_stack = *altstack;
	switched_stack = (ks->ks_flags & SA_ONSTACK) &&
		!(altstack->ss_flags & (SS_DISABLE | SS_ONSTACK));

	uk_pr_debug("tid %d handling signal %d, handler: 0x%lx, flags: 0x%lx\n",
		    uk_sys_gettid(), sig->siginfo.si_signo,
		    (__u64)ks->ks_handler,
		    ks->ks_flags);

	if (switched_stack) {
		UK_ASSERT(altstack->ss_sp);

		uk_pr_debug("Using altstack, ss_sp: 0x%lx, ss_size: 0x%lx\n",
			    (__u64)altstack->ss_sp, altstack->ss_size);

		ulsp = ukarch_gen_sp(altstack->ss_sp, altstack->ss_size);
		if ((unsigned int)altstack->ss_flags & SS_AUTODISARM)
			*altstack = (stack_t){ .ss_flags = SS_DISABLE };
		else
			altstack->ss_flags |= SS_ONSTACK;
	} else {
		ulsp = ALIGN_DOWN(uk_lcpu_regs_get(execenv->regs, SP),
				  UKARCH_SP_ALIGN);
		uk_pr_debug("Using the application stack @ 0x%lx\n", ulsp);
	}

	handler_ctx.handler_fn = ks->ks_handler;
	handler_ctx.handler_sp = (void *)ulsp;
	handler_ctx.signo = sig->siginfo.si_signo;

	if (ks->ks_flags & SA_SIGINFO) {
		handler_ctx.siginfo = &sig->siginfo;
		handler_ctx.ucontext = &ucontext;

		pprocess_signal_arch_set_ucontext(execenv, &ucontext);
		memcpy(&ucontext.uc_sigmask, saved_mask, sizeof(*saved_mask));
		ucontext.uc_stack = saved_stack;
		ucontext.uc_stack.ss_flags &= ~SS_ONSTACK;
		pprocess_signal_jmp_handler(&handler_ctx, execenv);
		if (pprocess_signal_arch_get_ucontext(&ucontext, execenv))
			uk_sigact_term(SIGSEGV);
		memcpy(saved_mask, &ucontext.uc_sigmask, sizeof(*saved_mask));
		uk_sigdelset(saved_mask, SIGKILL);
		uk_sigdelset(saved_mask, SIGSTOP);
		saved_stack = ucontext.uc_stack;
	} else {
		handler_ctx.siginfo = __NULL;
		handler_ctx.ucontext = __NULL;

		pprocess_signal_jmp_handler(&handler_ctx, execenv);
	}

	pprocess_signal_restore_altstack(&saved_stack,
		uk_lcpu_regs_get(execenv->regs, SP));
}

/* Deliver a signal to a thread. The caller must check that the signal
 * is not masked by the handling thread or ignored by the process or its
 * default disposition.
 */
static void do_deliver(struct posix_thread *pthread, struct uk_signal *sig,
		       struct ukarch_execenv *execenv,
		       const uk_sigset_t *restore_mask)
{
	struct posix_process *pproc;
	uk_sigset_t saved_mask;
	struct kern_sigaction action;
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

	handle_self(sig, &action, execenv, &saved_mask);

	uk_mutex_lock(&pprocess_mutex);
out_restore_mask:
	pthread->signal->mask = saved_mask;
#if CONFIG_ARCH_ARM_64
	if ((pthread->signal->sigqueue.pending | pproc->signal->sigqueue.pending) &
	    ~saved_mask)
		uk_store_n(&pthread->signal->notification, true);
#endif
	uk_mutex_unlock(&pprocess_mutex);
}

static void deliver_pending(struct posix_thread *thread, bool process_queue,
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
			do_deliver(thread, &delivery, execenv, NULL);
		}
	}
}

static void deliver_waited(struct posix_thread *thread,
			   struct ukarch_execenv *execenv)
{
	struct uk_signal_tdesc *td = thread->signal;
	struct uk_signal sig;
	uk_sigset_t saved_mask;
	bool restore_mask;

	uk_mutex_lock(&pprocess_mutex);
	if (!td->wait_pending) {
		uk_mutex_unlock(&pprocess_mutex);
		return;
	}
	sig.siginfo = td->wait_info;
	restore_mask = td->restore_mask;
	saved_mask = td->saved_mask;
	td->wait_pending = false;
	td->restore_mask = false;
	uk_mutex_unlock(&pprocess_mutex);
	do_deliver(thread, &sig, execenv, restore_mask ? &saved_mask : NULL);
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
#if CONFIG_ARCH_ARM_64
	uk_store_n(&pthread->signal->notification, false);
#endif

	pproc = uk_pprocess_current();
	UK_ASSERT(pproc);

	/* SIGSTOP / SIGCONT should have been already ignored by rt_sigaction */
	UK_ASSERT(!IS_PENDING(pproc->signal->sigqueue, SIGSTOP));
	UK_ASSERT(!IS_PENDING(pproc->signal->sigqueue, SIGCONT));

	deliver_waited(pthread, execenv);

	/* Deliver all pending signals of this process & thread */
	deliver_pending(pthread, true, execenv);
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
	struct posix_process *pproc;
	struct uk_signal sig = {0};

	/* Now we can enable IRQs */
	uk_lcpu_enable_irq();

	/* Get arg */
	error = (struct sys_error_desc *)arg;
	UK_ASSERT(error);

	/* Switch to uk sysregs */
	auxspcb = ukarch_auxsp_get_cb(error->auxsp);
	uk_lcpu_sysctx_load((struct uk_lcpu_sysctx *)auxspcb->uksysctx);

	/* Derive current process and thread */
	pproc = uk_pprocess_current();
	UK_ASSERT(pproc);

	pthread = uk_pthread_current();
	UK_ASSERT(pthread);

	pprocess_enter_application();

	/* If the application masks or ignores this signal, panic.
	 * A general-purpose OS would terminate the application.
	 * Being a unikernel, we treat this as a non-recoverable
	 * error instead.
	 */
	if (!pprocess_signal_is_deliverable(pthread, error->signum))
		goto err_panic;

	if (KERN_SIGACTION(pproc, error->signum)->ks_handler == SIG_DFL)
		goto err_panic;

	/* Prepare siginfo */
	set_siginfo_kill(error->signum, &sig.siginfo);
	if (error->code) {
		sig.siginfo.si_code = error->code;
		sig.siginfo.si_addr = (void *)error->vaddr;
	}

	/* Execute standard delivery path */
	do_deliver(pthread, &sig, ee, NULL);

	return;

err_panic:
	/* FIXME: Cascading faulting */
	UK_CRASH("Cannot deliver signal %d for fault at 0x%lx\n",
		 error->signum, error->vaddr);
}
