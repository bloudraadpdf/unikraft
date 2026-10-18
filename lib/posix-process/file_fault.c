/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/event.h>
#include <uk/nofault.h>
#include <uk/vmem.h>

#include "application.h"
#include "process.h"
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#endif

struct deferred_file_fault {
	__uptr auxsp;
	__vaddr_t address;
	unsigned int access;
};

static void file_fault_dispatch(struct ukarch_execenv *execenv, long arg)
{
	const struct deferred_file_fault *fault = (const void *)arg;
	struct ukarch_auxspcb *cb = ukarch_auxsp_get_cb(fault->auxsp);
	int rc;

	uk_lcpu_sysctx_load((struct uk_lcpu_sysctx *)cb->uksysctx);
	uk_lcpu_enable_irq();
	if (pprocess_application_context(execenv->regs))
		pprocess_enter_application();
	rc = uk_vma_populate_page(uk_vas_get_active(), fault->address,
				  fault->access);
	if (rc && !pprocess_application_context(execenv->regs) &&
	    !uk_nofault_fixup((struct uk_lcpu_regs *)execenv->regs))
		rc = 0;
	if (rc) {
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		if (pprocess_application_context(execenv->regs)) {
			struct sys_error_desc error = {
				.signum = rc == -ENOMEM ? SIGKILL :
					rc == -ENXIO ? SIGBUS : SIGSEGV,
				.code = rc == -ENXIO ? BUS_ADRERR : 0,
				.auxsp = fault->auxsp,
				.vaddr = fault->address,
			};

			sys_error_handler(execenv, (long)&error);
		} else
#endif
			UK_CRASH("File page fault at 0x%lx failed: %d\n",
				 fault->address, rc);
	}
	pprocess_execenv_load((long)execenv);
}

static int deferred_file_fault(void *data)
{
	struct uk_lcpu_except_err_ctx *ctx = data;
	struct uk_lcpu_regs *regs = uk_lcpu_except_err_ctx_get_regs(ctx);
	struct uk_thread *thread = uk_thread_current();
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);
	struct ukarch_auxspcb *cb;
	struct deferred_file_fault *fault;
	struct ukarch_ctx target;
	__uptr sp;

	if (uk_lcpu_except_err_ctx_get_handler_err(ctx) != -EAGAIN ||
	    !thread || !(thread->flags & UK_THREADF_UKTLS) || !auxsp ||
	    !pprocess_regs_irq_enabled(regs))
		return UK_EVENT_NOT_HANDLED;
	cb = ukarch_auxsp_get_cb(auxsp);
	sp = pprocess_application_context(regs) ?
		ukarch_auxspcb_get_curr_fp(cb) : uk_lcpu_regs_get(regs, SP);
	sp = ALIGN_DOWN(sp - sizeof(*fault), UKARCH_EXECENV_END_ALIGN);
	fault = (void *)sp;
	fault->auxsp = auxsp;
	fault->address = uk_lcpu_except_err_ctx_get_fault_addr(ctx);
	fault->access = uk_vmem_arch_fault_type(ctx) & UK_VMA_FAULT_ACCESSTYPE;
	ukarch_ctx_init_ehtrampo(&target, regs, sp, file_fault_dispatch,
				 (long)fault);
	uk_lcpu_regs_set(regs, PC, target.ip);
	uk_lcpu_regs_set(regs, SP, target.sp);
	pprocess_regs_disable_irq(regs);
	return UK_EVENT_HANDLED;
}

UK_EVENT_HANDLER_PRIO(UK_LCPU_EXCEPT_EVENT_ERR_PAGE_FAULT,
		      deferred_file_fault, UK_PRIO_BEFORE(UK_PRIO_LATEST));
