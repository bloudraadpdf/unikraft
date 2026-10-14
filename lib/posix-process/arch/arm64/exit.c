/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/arch/ctx.h>
#include <uk/event.h>
#include <uk/lcpu.h>
#include <uk/plat/common/sections.h>
#include <uk/plat/config.h>
#include <uk/sched.h>
#include <uk/syscall.h>

#include "../../process.h"

void pprocess_exit_trampoline(void);

static bool application_context(void *regs)
{
	__uptr pc = uk_lcpu_regs_get(regs, PC);
	__uptr sp = uk_lcpu_regs_get(regs, SP);
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);

	return auxsp && (pc < __TEXT || pc >= __ETEXT) &&
		!SP_IN_AUXSP(sp, auxsp);
}

void __noreturn pprocess_execenv_load(long address)
{
	struct ukarch_execenv *execenv = (struct ukarch_execenv *)address;

	uk_lcpu_disable_irq();
	if (application_context(execenv->regs))
		pprocess_enter_application();
	ukarch_execenv_load(address);
}

static int application_exit_irq(void *data)
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
	return UK_EVENT_NOT_HANDLED;
}

UK_EVENT_HANDLER_PRIO(UK_LCPU_EXCEPT_EVENT_IRQ, application_exit_irq,
		      UK_PRIO_EARLIEST);
