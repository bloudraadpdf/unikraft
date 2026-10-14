/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include <uk/process.h>
#include <uk/syscall.h>

#include "signal.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
static int set_altstack(stack_t *current, const stack_t *stack)
{
	stack_t next = *stack;
	unsigned int mode = (unsigned int)next.ss_flags & ~SS_AUTODISARM;

	if (current->ss_flags & SS_ONSTACK)
		return -EPERM;
	if (mode != 0 && mode != SS_DISABLE && mode != SS_ONSTACK)
		return -EINVAL;
	if (mode == SS_DISABLE) {
		next.ss_sp = NULL;
		next.ss_size = 0;
	} else {
		if (next.ss_size < MINSIGSTKSZ)
			return -ENOMEM;
		next.ss_flags &= ~SS_ONSTACK;
	}
	*current = next;
	return 0;
}

static void update_stack_position(stack_t *stack, __uptr sp)
{
	stack->ss_flags &= ~SS_ONSTACK;
	if (!((unsigned int)stack->ss_flags & (SS_DISABLE | SS_AUTODISARM)) &&
	    sp > (__uptr)stack->ss_sp && sp - (__uptr)stack->ss_sp <= stack->ss_size)
		stack->ss_flags |= SS_ONSTACK;
}

#if CONFIG_ARCH_ARM_64
void pprocess_signal_altstack_update(__uptr sp)
{
	update_stack_position(&uk_pthread_current()->signal->altstack, sp);
}
#endif

void pprocess_signal_restore_altstack(const stack_t *stack, __uptr sp)
{
	stack_t *current = &uk_pthread_current()->signal->altstack;

	update_stack_position(current, sp);
	(void)set_altstack(current, stack);
	update_stack_position(current, sp);
}

#if CONFIG_ARCH_ARM_64
UK_SYSCALL_R_E_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
#else
UK_SYSCALL_R_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
#endif
{
	stack_t *current = &uk_pthread_current()->signal->altstack;
	stack_t previous;
	int ret;

#if CONFIG_ARCH_ARM_64
	if (execenv)
		update_stack_position(current, uk_lcpu_regs_get(execenv->regs, SP));
#endif
	previous = *current;

	if (ss) {
		ret = set_altstack(current, ss);
		if (ret)
			return ret;
	}
	if (old_ss)
		*old_ss = previous;
	return 0;
}

#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#if CONFIG_ARCH_ARM_64
UK_SYSCALL_R_E_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
#else
UK_SYSCALL_R_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
#endif
{
	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
