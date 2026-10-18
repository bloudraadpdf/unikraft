/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include <uk/nofault.h>
#if CONFIG_LIBUKPAGING
#include <uk/paging.h>
#endif
#include <uk/process.h>
#include <uk/sched.h>
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

struct pprocess_signal_stack
pprocess_signal_stack_prepare(const struct kern_sigaction *action,
			      __uptr sp, __sz redzone)
{
	stack_t *current = &uk_pthread_current()->signal->altstack;
	struct pprocess_signal_stack stack;

	update_stack_position(current, sp);
	stack.previous = *current;
	stack.on_altstack = current->ss_flags & SS_ONSTACK;
	stack.sp = sp >= redzone ? sp - redzone : 0;
	if ((action->ks_flags & SA_ONSTACK) &&
	    !(current->ss_flags & (SS_DISABLE | SS_ONSTACK))) {
		stack.sp = (__uptr)current->ss_sp <= __PTR_MAX - current->ss_size ?
			(__uptr)current->ss_sp + current->ss_size : 0;
		stack.on_altstack = true;
		if ((unsigned int)current->ss_flags & SS_AUTODISARM)
			*current = (stack_t){ .ss_flags = SS_DISABLE };
		else
			current->ss_flags |= SS_ONSTACK;
	}
	return stack;
}

int pprocess_signal_stack_reserve(const struct pprocess_signal_stack *stack,
				  __uptr bottom, __uptr top)
{
	__sz length;

	if (bottom >= top || top > stack->sp)
		return -EFAULT;
	if (stack->on_altstack &&
	    (bottom < (__uptr)stack->previous.ss_sp ||
	     top - (__uptr)stack->previous.ss_sp > stack->previous.ss_size))
		return -EFAULT;
	length = top - bottom;
#if CONFIG_LIBUKPAGING
	if (!uk_paging_vaddr_range_isvalid(bottom, length))
		return -EFAULT;
#endif
	if (uk_nofault_probe_rw(bottom, length, 0) == length)
		return 0;
#if CONFIG_LIBUKVMEM && CONFIG_LIBPOSIX_PROCESS_EXECVE
	if (!stack->on_altstack && !pprocess_memory_stack(bottom) &&
	    uk_nofault_probe_rw(bottom, length, 0) == length)
		return 0;
#endif
	return -EFAULT;
}

void pprocess_signal_bad_stack(void)
{
	pprocess_exit(uk_pprocess_current(), POSIX_PROCESS_KILLED, SIGSEGV);
	uk_sched_thread_exit();
}

void pprocess_signal_restore_altstack(const stack_t *stack, __uptr sp)
{
	stack_t *current = &uk_pthread_current()->signal->altstack;

	update_stack_position(current, sp);
	(void)set_altstack(current, stack);
	update_stack_position(current, sp);
}

UK_SYSCALL_R_E_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
{
	stack_t *current = &uk_pthread_current()->signal->altstack;
	stack_t previous, next;
	int ret;

	if (execenv)
		update_stack_position(current, uk_lcpu_regs_get(execenv->regs, SP));
	previous = *current;

	if (ss) {
		if (!uk_nofault_try_read(next, ss))
			return -EFAULT;
		ret = set_altstack(current, &next);
		if (ret)
			return ret;
	}
	if (old_ss && !uk_nofault_try_write(previous, old_ss))
		return -EFAULT;
	return 0;
}

#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */

UK_SYSCALL_R_E_DEFINE(int, sigaltstack, const stack_t *, ss,
		    stack_t *, old_ss)
{
	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
