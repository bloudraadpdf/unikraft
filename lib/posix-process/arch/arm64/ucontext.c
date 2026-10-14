/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>

#include <uk/essentials.h>
#include <uk/syscall.h>
#include <uk/sched.h>
#include "../../signal/signal.h"
#if CONFIG_LIBUKPLAT_NATIVE_ECTX
#include <uk/plat/native/ectx.h>
#endif

UK_CTASSERT(__offsetof(ucontext_t, uc_mcontext) == 176);

static int restore_extended_context(ucontext_t *ucontext,
				    struct ukarch_execenv *execenv)
{
	const unsigned char *position = (const void *)ucontext->uc_mcontext.__reserved;
	size_t left = sizeof(ucontext->uc_mcontext.__reserved);
#if CONFIG_LIBUKPLAT_NATIVE_ECTX
	const struct fpsimd_context *fp = NULL;
	struct uk_plat_native_ectx *state = (void *)execenv->ectx;
#else
	(void)execenv;
#endif

	while (left >= sizeof(struct _aarch64_ctx)) {
		const struct _aarch64_ctx *head = (const void *)position;

		if (!head->magic && !head->size) {
#if CONFIG_LIBUKPLAT_NATIVE_ECTX
			if (!fp)
				return -EINVAL;
			memcpy(state->fpregs, fp->vregs, sizeof(state->fpregs));
			state->fpsr = fp->fpsr;
			state->fpcr = fp->fpcr;
#endif
			return 0;
		}
		if (head->size < sizeof(*head) || head->size > left ||
		    (head->size & 15))
			return -EINVAL;
		switch (head->magic) {
#if CONFIG_LIBUKPLAT_NATIVE_ECTX
		case FPSIMD_MAGIC:
			if (fp || head->size != sizeof(*fp))
				return -EINVAL;
			fp = (const void *)head;
			break;
#endif
		case ESR_MAGIC:
			break;
		default:
			return -EINVAL;
		}
		left -= head->size;
		position += head->size;
	}
	return -EINVAL;
}

void pprocess_signal_arch_set_ucontext(struct ukarch_execenv *execenv,
				       ucontext_t *ucontext)
{
	UK_ASSERT(execenv);
	UK_ASSERT(ucontext);

	memset(ucontext, 0, sizeof(*ucontext));
#if CONFIG_LIBUKPLAT_NATIVE_ECTX
	struct fpsimd_context *fp = (void *)ucontext->uc_mcontext.__reserved;
	const struct uk_plat_native_ectx *state = (const void *)execenv->ectx;

	UK_CTASSERT(sizeof(fp->vregs) == sizeof(state->fpregs));
	UK_CTASSERT(sizeof(*fp) == 528);
	fp->head.magic = FPSIMD_MAGIC;
	fp->head.size = sizeof(*fp);
	fp->fpsr = state->fpsr;
	fp->fpcr = state->fpcr;
	memcpy(fp->vregs, state->fpregs, sizeof(fp->vregs));
#endif

	ucontext->uc_mcontext.regs[0] = uk_lcpu_regs_get(execenv->regs, X0);
	ucontext->uc_mcontext.regs[1] = uk_lcpu_regs_get(execenv->regs, X1);
	ucontext->uc_mcontext.regs[2] = uk_lcpu_regs_get(execenv->regs, X2);
	ucontext->uc_mcontext.regs[3] = uk_lcpu_regs_get(execenv->regs, X3);
	ucontext->uc_mcontext.regs[4] = uk_lcpu_regs_get(execenv->regs, X4);
	ucontext->uc_mcontext.regs[5] = uk_lcpu_regs_get(execenv->regs, X5);
	ucontext->uc_mcontext.regs[6] = uk_lcpu_regs_get(execenv->regs, X6);
	ucontext->uc_mcontext.regs[7] = uk_lcpu_regs_get(execenv->regs, X7);
	ucontext->uc_mcontext.regs[8] = uk_lcpu_regs_get(execenv->regs, X8);
	ucontext->uc_mcontext.regs[9] = uk_lcpu_regs_get(execenv->regs, X9);
	ucontext->uc_mcontext.regs[10] = uk_lcpu_regs_get(execenv->regs, X10);
	ucontext->uc_mcontext.regs[11] = uk_lcpu_regs_get(execenv->regs, X11);
	ucontext->uc_mcontext.regs[12] = uk_lcpu_regs_get(execenv->regs, X12);
	ucontext->uc_mcontext.regs[13] = uk_lcpu_regs_get(execenv->regs, X13);
	ucontext->uc_mcontext.regs[14] = uk_lcpu_regs_get(execenv->regs, X14);
	ucontext->uc_mcontext.regs[15] = uk_lcpu_regs_get(execenv->regs, X15);
	ucontext->uc_mcontext.regs[16] = uk_lcpu_regs_get(execenv->regs, X16);
	ucontext->uc_mcontext.regs[17] = uk_lcpu_regs_get(execenv->regs, X17);
	ucontext->uc_mcontext.regs[18] = uk_lcpu_regs_get(execenv->regs, X18);
	ucontext->uc_mcontext.regs[19] = uk_lcpu_regs_get(execenv->regs, X19);
	ucontext->uc_mcontext.regs[20] = uk_lcpu_regs_get(execenv->regs, X20);
	ucontext->uc_mcontext.regs[21] = uk_lcpu_regs_get(execenv->regs, X21);
	ucontext->uc_mcontext.regs[22] = uk_lcpu_regs_get(execenv->regs, X22);
	ucontext->uc_mcontext.regs[23] = uk_lcpu_regs_get(execenv->regs, X23);
	ucontext->uc_mcontext.regs[24] = uk_lcpu_regs_get(execenv->regs, X24);
	ucontext->uc_mcontext.regs[25] = uk_lcpu_regs_get(execenv->regs, X25);
	ucontext->uc_mcontext.regs[26] = uk_lcpu_regs_get(execenv->regs, X26);
	ucontext->uc_mcontext.regs[27] = uk_lcpu_regs_get(execenv->regs, X27);
	ucontext->uc_mcontext.regs[28] = uk_lcpu_regs_get(execenv->regs, X28);
	ucontext->uc_mcontext.regs[29] = uk_lcpu_regs_get(execenv->regs, X29);

	ucontext->uc_mcontext.regs[30] = uk_lcpu_regs_get(execenv->regs, LR);
	ucontext->uc_mcontext.sp = uk_lcpu_regs_get(execenv->regs, SP);
	ucontext->uc_mcontext.pc = uk_lcpu_regs_get(execenv->regs, PC);
	ucontext->uc_mcontext.pstate = uk_lcpu_regs_get(execenv->regs, SPSR_EL1);
}

int pprocess_signal_arch_get_ucontext(ucontext_t *ucontext,
				       struct ukarch_execenv *execenv)
{
	UK_ASSERT(ucontext);
	UK_ASSERT(execenv);

	if (restore_extended_context(ucontext, execenv))
		return -EINVAL;

	uk_lcpu_regs_set(execenv->regs, X0, ucontext->uc_mcontext.regs[0]);
	uk_lcpu_regs_set(execenv->regs, X1, ucontext->uc_mcontext.regs[1]);
	uk_lcpu_regs_set(execenv->regs, X2, ucontext->uc_mcontext.regs[2]);
	uk_lcpu_regs_set(execenv->regs, X3, ucontext->uc_mcontext.regs[3]);
	uk_lcpu_regs_set(execenv->regs, X4, ucontext->uc_mcontext.regs[4]);
	uk_lcpu_regs_set(execenv->regs, X5, ucontext->uc_mcontext.regs[5]);
	uk_lcpu_regs_set(execenv->regs, X6, ucontext->uc_mcontext.regs[6]);
	uk_lcpu_regs_set(execenv->regs, X7, ucontext->uc_mcontext.regs[7]);
	uk_lcpu_regs_set(execenv->regs, X8, ucontext->uc_mcontext.regs[8]);
	uk_lcpu_regs_set(execenv->regs, X9, ucontext->uc_mcontext.regs[9]);
	uk_lcpu_regs_set(execenv->regs, X10, ucontext->uc_mcontext.regs[10]);
	uk_lcpu_regs_set(execenv->regs, X11, ucontext->uc_mcontext.regs[11]);
	uk_lcpu_regs_set(execenv->regs, X12, ucontext->uc_mcontext.regs[12]);
	uk_lcpu_regs_set(execenv->regs, X13, ucontext->uc_mcontext.regs[13]);
	uk_lcpu_regs_set(execenv->regs, X14, ucontext->uc_mcontext.regs[14]);
	uk_lcpu_regs_set(execenv->regs, X15, ucontext->uc_mcontext.regs[15]);
	uk_lcpu_regs_set(execenv->regs, X16, ucontext->uc_mcontext.regs[16]);
	uk_lcpu_regs_set(execenv->regs, X17, ucontext->uc_mcontext.regs[17]);
	uk_lcpu_regs_set(execenv->regs, X18, ucontext->uc_mcontext.regs[18]);
	uk_lcpu_regs_set(execenv->regs, X19, ucontext->uc_mcontext.regs[19]);
	uk_lcpu_regs_set(execenv->regs, X20, ucontext->uc_mcontext.regs[20]);
	uk_lcpu_regs_set(execenv->regs, X21, ucontext->uc_mcontext.regs[21]);
	uk_lcpu_regs_set(execenv->regs, X22, ucontext->uc_mcontext.regs[22]);
	uk_lcpu_regs_set(execenv->regs, X23, ucontext->uc_mcontext.regs[23]);
	uk_lcpu_regs_set(execenv->regs, X24, ucontext->uc_mcontext.regs[24]);
	uk_lcpu_regs_set(execenv->regs, X25, ucontext->uc_mcontext.regs[25]);
	uk_lcpu_regs_set(execenv->regs, X26, ucontext->uc_mcontext.regs[26]);
	uk_lcpu_regs_set(execenv->regs, X27, ucontext->uc_mcontext.regs[27]);
	uk_lcpu_regs_set(execenv->regs, X28, ucontext->uc_mcontext.regs[28]);
	uk_lcpu_regs_set(execenv->regs, X29, ucontext->uc_mcontext.regs[29]);

	uk_lcpu_regs_set(execenv->regs, LR, ucontext->uc_mcontext.regs[30]);
	uk_lcpu_regs_set(execenv->regs, SP, ucontext->uc_mcontext.sp);
	uk_lcpu_regs_set(execenv->regs, PC, ucontext->uc_mcontext.pc);
	uk_lcpu_regs_set(execenv->regs, SPSR_EL1, ucontext->uc_mcontext.pstate);
	return 0;
}

struct arm64_signal_frame {
	siginfo_t info;
	ucontext_t context;
	struct {
		__uptr fp;
		__uptr lr;
	} previous;
};

UK_CTASSERT(__offsetof(struct arm64_signal_frame, context) == 128);
UK_CTASSERT(sizeof(struct arm64_signal_frame) < MINSIGSTKSZ);
UK_CTASSERT(IS_ALIGNED(sizeof(struct arm64_signal_frame), UKARCH_SP_ALIGN));

void pprocess_signal_return(void);

void pprocess_signal_arch_prepare(struct uk_signal *signal,
	const struct kern_sigaction *action, struct ukarch_execenv *execenv,
	const uk_sigset_t *saved_mask)
{
	struct uk_signal_tdesc *state = uk_pthread_current()->signal;
	__uptr sp = uk_lcpu_regs_get(execenv->regs, SP);
	struct arm64_signal_frame *frame;
	stack_t saved_stack;
	bool on_altstack;

	pprocess_signal_altstack_update(sp);
	saved_stack = state->altstack;
	on_altstack = saved_stack.ss_flags & SS_ONSTACK;
	if ((action->ks_flags & SA_ONSTACK) &&
	    !(saved_stack.ss_flags & (SS_DISABLE | SS_ONSTACK))) {
		sp = (__uptr)saved_stack.ss_sp + saved_stack.ss_size;
		on_altstack = true;
		if ((unsigned int)saved_stack.ss_flags & SS_AUTODISARM)
			state->altstack = (stack_t){ .ss_flags = SS_DISABLE };
		else
			state->altstack.ss_flags |= SS_ONSTACK;
	}
	if (sp < sizeof(*frame) || (on_altstack &&
	    ALIGN_DOWN(sp - sizeof(*frame), UKARCH_SP_ALIGN) < (__uptr)saved_stack.ss_sp)) {
		pprocess_exit(uk_pprocess_current(), POSIX_PROCESS_KILLED, SIGSEGV);
		uk_sched_thread_exit();
	}
	sp = ALIGN_DOWN(sp - sizeof(*frame), UKARCH_SP_ALIGN);
	frame = (void *)sp;
	frame->previous.fp = uk_lcpu_regs_get(execenv->regs, X29);
	frame->previous.lr = uk_lcpu_regs_get(execenv->regs, LR);
	frame->info = signal->siginfo;
	pprocess_signal_arch_set_ucontext(execenv, &frame->context);
	memcpy(&frame->context.uc_sigmask, saved_mask, sizeof(*saved_mask));
	frame->context.uc_stack = saved_stack;
	if (signal->siginfo.si_code > 0 &&
	    (signal->siginfo.si_signo == SIGBUS || signal->siginfo.si_signo == SIGSEGV))
		frame->context.uc_mcontext.fault_address = (__uptr)signal->siginfo.si_addr;
	uk_lcpu_regs_set(execenv->regs, PC, (__uptr)action->ks_handler);
	uk_lcpu_regs_set(execenv->regs, SP, sp);
	uk_lcpu_regs_set(execenv->regs, X0, signal->siginfo.si_signo);
	uk_lcpu_regs_set(execenv->regs, X1, (__uptr)&frame->info);
	uk_lcpu_regs_set(execenv->regs, X2, (__uptr)&frame->context);
	uk_lcpu_regs_set(execenv->regs, X29, (__uptr)&frame->previous);
	uk_lcpu_regs_set(execenv->regs, LR, (__uptr)(action->ks_flags & SA_RESTORER ?
		action->ks_restorer : pprocess_signal_return));
}

UK_LLSYSCALL_R_E_DEFINE(long, rt_sigreturn)
{
	struct arm64_signal_frame *frame = (void *)uk_lcpu_regs_get(execenv->regs, SP);
	struct posix_thread *thread = uk_pthread_current();
	struct posix_process *process = thread->process;
	uk_sigset_t mask;

	if (!IS_ALIGNED((__uptr)frame, UKARCH_SP_ALIGN) ||
	    pprocess_signal_arch_get_ucontext(&frame->context, execenv)) {
		pprocess_exit(process, POSIX_PROCESS_KILLED, SIGSEGV);
		uk_sched_thread_exit();
	}
	memcpy(&mask, &frame->context.uc_sigmask, sizeof(mask));
	uk_sigdelset(&mask, SIGKILL);
	uk_sigdelset(&mask, SIGSTOP);
	uk_mutex_lock(&pprocess_mutex);
	thread->signal->mask = mask;
	if ((thread->signal->sigqueue.pending | process->signal->sigqueue.pending) & ~mask)
		uk_store_n(&thread->signal->notification, true);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_signal_restore_altstack(&frame->context.uc_stack,
		uk_lcpu_regs_get(execenv->regs, SP));
	return uk_lcpu_regs_get(execenv->regs, X0);
}
