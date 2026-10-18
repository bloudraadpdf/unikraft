/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/arch/ctx.h>
#include <uk/event.h>
#include <uk/lcpu.h>
#include <uk/plat/config.h>
#include <uk/plat/time.h>
#include <uk/sched.h>
#include <uk/syscall.h>

#include "process.h"
#include "application.h"
#include "rseq.h"
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#endif

void pprocess_exit_trampoline(void);

#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
#define APPLICATION_QUANTUM_NS (UKARCH_NSEC_PER_SEC / 100)
#endif

static void prepare_application(struct ukarch_execenv *execenv)
{
#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
	struct posix_thread *thread = uk_pthread_current();
	__nsec now;
#endif

	pprocess_rseq_interrupt(execenv);
	uk_lcpu_disable_irq();
	if (!pprocess_application_context(execenv->regs))
		return;
	for (;;) {
		pprocess_enter_application();
		if (uk_sched_thread_affinity_pending(uk_thread_current())) {
			uk_lcpu_enable_irq();
			uk_sched_yield();
			uk_lcpu_disable_irq();
			continue;
		}
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		if (uk_load_n(&uk_pthread_current()->signal->notification)) {
			uk_lcpu_enable_irq();
			pprocess_signal_deliver(execenv);
			uk_lcpu_disable_irq();
			continue;
		}
#endif
#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
		now = ukplat_monotonic_clock();
		if (uk_sched_has_higher_priority()) {
			uk_lcpu_enable_irq();
			uk_sched_yield();
			uk_lcpu_disable_irq();
			continue;
		}
		if (thread->application_deadline &&
		    thread->application_deadline <= now) {
			thread->application_deadline = 0;
			uk_lcpu_enable_irq();
			uk_sched_yield();
			uk_lcpu_disable_irq();
			continue;
		}
		if (!thread->application_deadline)
			thread->application_deadline =
				now + APPLICATION_QUANTUM_NS;
		ukplat_time_arm_timer(thread->application_deadline);
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
void pprocess_signal_notify(struct posix_thread *thread)
{
	uk_store_n(&thread->signal->notification, true);
	uk_sched_thread_notify(thread->thread);
}

#endif

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL || CONFIG_LIBPOSIX_PROCESS_PREEMPT ||       \
	CONFIG_HAVE_SMP
static void application_irq_dispatch(struct ukarch_execenv *execenv, long auxsp)
{
	struct ukarch_auxspcb *cb = ukarch_auxsp_get_cb(auxsp);

	uk_lcpu_sysctx_load((struct uk_lcpu_sysctx *)cb->uksysctx);
	uk_lcpu_enable_irq();
	pprocess_rseq_interrupt(execenv);
#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
	uk_pthread_current()->application_deadline = 0;
	uk_sched_yield();
#endif
	pprocess_enter_application();
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_deliver(execenv);
#endif
	pprocess_execenv_load((long)execenv);
}

static void prepare_application_irq(struct uk_lcpu_regs *regs)
{
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);
	struct ukarch_auxspcb *cb = ukarch_auxsp_get_cb(auxsp);
	struct ukarch_ctx target;

	ukarch_ctx_init_ehtrampo(&target, regs,
			       ukarch_auxspcb_get_curr_fp(cb),
			       application_irq_dispatch, auxsp);
	uk_lcpu_regs_set(regs, PC, target.ip);
	uk_lcpu_regs_set(regs, SP, target.sp);
	pprocess_regs_disable_irq(regs);
}
#endif

static int application_irq(void *data)
{
	struct uk_lcpu_except_irq_ctx *ctx = data;
	struct uk_lcpu_regs *regs = uk_lcpu_except_irq_ctx_get_regs(ctx);
	struct uk_thread *thread = uk_thread_current();
	struct posix_thread *pthread;

	if (!thread || !(thread->flags & UK_THREADF_UKTLS) ||
	    !pprocess_application_context(regs))
		return UK_EVENT_NOT_HANDLED;
	pthread = uk_thread_uktls_var(thread, pthread_self);
	if (pthread && pprocess_thread_exit_requested(pthread)) {
#if CONFIG_ARCH_ARM_64
		uk_lcpu_regs_set(regs, X8, SYS_exit);
#else
		uk_lcpu_regs_set(regs, RAX, SYS_exit);
#endif
		uk_lcpu_regs_set(regs, PC, (__uptr)pprocess_exit_trampoline);
	}
#if CONFIG_HAVE_SMP
	else if (pthread && uk_sched_thread_affinity_pending(thread))
		prepare_application_irq(regs);
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	else if (pthread && uk_load_n(&pthread->signal->notification)) {
		prepare_application_irq(regs);
	}
#endif
#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
	else if (pthread &&
		 ukplat_monotonic_clock() >= pthread->application_deadline) {
		prepare_application_irq(regs);
	}
#endif
	return UK_EVENT_NOT_HANDLED;
}

UK_EVENT_HANDLER_PRIO(UK_LCPU_EXCEPT_EVENT_IRQ, application_irq,
		      UK_PRIO_EARLIEST);
