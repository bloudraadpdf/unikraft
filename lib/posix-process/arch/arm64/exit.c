/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/arch/ctx.h>
#include <uk/event.h>
#include <uk/isr/string.h>
#include <uk/lcpu.h>
#include <uk/plat/common/sections.h>
#include <uk/plat/config.h>
#include <uk/sched.h>
#include <uk/syscall.h>

#include "../../process.h"
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "../../signal/signal.h"
#endif

#define ARM64_IRQ_MASK UK_BIT(7)

void pprocess_exit_trampoline(void);

static bool application_context(void *regs)
{
	__uptr pc = uk_lcpu_regs_get(regs, PC);
	__uptr sp = uk_lcpu_regs_get(regs, SP);
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);

	return auxsp && (pc < __TEXT || pc >= __ETEXT) &&
		!SP_IN_AUXSP(sp, auxsp);
}

static void prepare_application(struct ukarch_execenv *execenv)
{
	uk_lcpu_disable_irq();
	if (!application_context(execenv->regs))
		return;
	for (;;) {
		pprocess_enter_application();
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		if (uk_load_n(&uk_pthread_current()->signal->notification)) {
			uk_lcpu_enable_irq();
			pprocess_signal_deliver(execenv);
			uk_lcpu_disable_irq();
			continue;
		}
#endif
		return;
	}
}

void __noreturn pprocess_execenv_load(long address)
{
	prepare_application((struct ukarch_execenv *)address);
	ukarch_execenv_load(address);
}

static void application_syscall_return(struct uk_syscall_exit_ctx *ctx)
{
	if (ctx->execenv && (ctx->flags & UK_SYSCALL_EXIT_CTX_BINARY_SYSCALL))
		prepare_application(ctx->execenv);
}

uk_syscall_exittab_prio(application_syscall_return, UK_PRIO_LATEST);

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
void __noreturn pprocess_signal_handler_enter(
	struct pprocess_signal_handler_ctx *handler,
	struct ukarch_execenv *original, __uptr kernel_sp,
	__uptr kernel_fp, __uptr return_pc)
{
	struct ukarch_execenv execenv;

	memcpy_isr(&execenv, original, sizeof(execenv));
	uk_lcpu_regs_set(execenv.regs, PC, (__uptr)handler->handler_fn);
	uk_lcpu_regs_set(execenv.regs, SP, (__uptr)handler->handler_sp);
	uk_lcpu_regs_set(execenv.regs, X0, handler->signo);
	uk_lcpu_regs_set(execenv.regs, X1, (__uptr)handler->siginfo);
	uk_lcpu_regs_set(execenv.regs, X2, (__uptr)handler->ucontext);
	uk_lcpu_regs_set(execenv.regs, X21, kernel_sp);
	uk_lcpu_regs_set(execenv.regs, X29, kernel_fp);
	uk_lcpu_regs_set(execenv.regs, LR, return_pc);
	pprocess_execenv_load((long)&execenv);
}

void pprocess_signal_notify(struct posix_thread *thread)
{
	uk_store_n(&thread->signal->notification, true);
#if CONFIG_HAVE_SMP
	if (thread->thread->sched) {
		unsigned int count = 1;
		int rc = uk_lcpu_wakeup(&thread->thread->sched->cpu_index, &count);

		if (rc || count != 1)
			UK_CRASH("Signal CPU notification failed: %d\n", rc);
	}
#endif
}

static void application_signal_dispatch(struct ukarch_execenv *execenv, long auxsp)
{
	struct ukarch_auxspcb *cb = ukarch_auxsp_get_cb(auxsp);

	uk_lcpu_sysctx_load((struct uk_lcpu_sysctx *)cb->uksysctx);
	uk_lcpu_enable_irq();
	pprocess_enter_application();
	pprocess_signal_deliver(execenv);
	pprocess_execenv_load((long)execenv);
}

static void prepare_signal_irq(struct uk_lcpu_regs *regs)
{
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);
	struct ukarch_auxspcb *cb = ukarch_auxsp_get_cb(auxsp);
	struct ukarch_ctx target;

	ukarch_ctx_init_ehtrampo(&target, regs,
		ukarch_auxspcb_get_curr_fp(cb), application_signal_dispatch, auxsp);
	uk_lcpu_regs_set(regs, PC, target.ip);
	uk_lcpu_regs_set(regs, SP, target.sp);
	uk_lcpu_regs_set(regs, SPSR_EL1,
		uk_lcpu_regs_get(regs, SPSR_EL1) | ARM64_IRQ_MASK);
}
#endif

static int application_irq(void *data)
{
	struct uk_lcpu_except_irq_ctx *ctx = data;
	struct uk_lcpu_regs *regs = uk_lcpu_except_irq_ctx_get_regs(ctx);
	struct uk_thread *thread = uk_thread_current();
	struct posix_thread *pthread;

	if (!thread || !(thread->flags & UK_THREADF_UKTLS) ||
	    !application_context(regs))
		return UK_EVENT_NOT_HANDLED;
	pthread = uk_thread_uktls_var(thread, pthread_self);
	if (pthread && pprocess_thread_exit_requested(pthread)) {
		uk_lcpu_regs_set(regs, X8, SYS_exit);
		uk_lcpu_regs_set(regs, PC, (__uptr)pprocess_exit_trampoline);
	}
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	else if (pthread && uk_load_n(&pthread->signal->notification)) {
		prepare_signal_irq(regs);
	}
#endif
	return UK_EVENT_NOT_HANDLED;
}

UK_EVENT_HANDLER_PRIO(UK_LCPU_EXCEPT_EVENT_IRQ, application_irq,
		      UK_PRIO_EARLIEST);
