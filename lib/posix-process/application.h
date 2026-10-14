/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSIX_PROCESS_APPLICATION_H
#define POSIX_PROCESS_APPLICATION_H

#include <uk/arch/ctx.h>
#include <uk/lcpu.h>
#include <uk/plat/common/sections.h>
#include <uk/plat/config.h>
#if CONFIG_ARCH_X86_64
#include <uk/arch/x86_64.h>
#endif

static inline bool pprocess_application_context(void *regs)
{
	__uptr pc = uk_lcpu_regs_get(regs, PC);
	__uptr sp = uk_lcpu_regs_get(regs, SP);
	__uptr auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);

	return auxsp && (pc < __TEXT || pc >= __ETEXT) &&
		!SP_IN_AUXSP(sp, auxsp);
}

static inline bool pprocess_regs_irq_enabled(void *regs)
{
#if CONFIG_ARCH_ARM_64
	return !(uk_lcpu_regs_get(regs, SPSR_EL1) & UK_BIT(7));
#else
	return uk_lcpu_regs_get(regs, RFLAGS) & UK_ARCH_X86_64_RFLAGS_IF;
#endif
}

static inline void pprocess_regs_disable_irq(void *regs)
{
#if CONFIG_ARCH_ARM_64
	uk_lcpu_regs_set(regs, SPSR_EL1,
			 uk_lcpu_regs_get(regs, SPSR_EL1) | UK_BIT(7));
#else
	uk_lcpu_regs_set(regs, RFLAGS,
			 uk_lcpu_regs_get(regs, RFLAGS) &
			 ~UK_ARCH_X86_64_RFLAGS_IF);
#endif
}

#endif
