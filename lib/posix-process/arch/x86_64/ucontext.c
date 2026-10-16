/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <uk/sched.h>
#include <uk/nofault.h>
#include <uk/plat/native/ectx.h>
#include "../../signal/signal.h"

#include <uk/arch/ctx.h>
#include <uk/lcpu.h>
#include <uk/syscall.h>

struct x86_signal_context {
	unsigned long uc_flags;
	ucontext_t *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	uk_sigset_t uc_sigmask;
};

struct x86_signal_frame {
	__uptr restorer;
	struct x86_signal_context context;
	siginfo_t info;
};

struct xstate_metadata {
	__u32 magic;
	__u32 extended_size;
	__u64 features;
	__u32 state_size;
	__u32 reserved[7];
};

#define XSTATE_MAGIC 0x46505853U
#define XSTATE_END 0x46505845U
#define SIGNAL_RFLAGS 0x50dd5UL

UK_CTASSERT(sizeof(struct xstate_metadata) == 48);
UK_CTASSERT(sizeof(struct x86_signal_context) == 304);
UK_CTASSERT(__offsetof(struct x86_signal_context, uc_sigmask) ==
	    __offsetof(ucontext_t, uc_sigmask));
UK_CTASSERT(__offsetof(struct x86_signal_frame, info) == 312);

static void set_ucontext(struct ukarch_execenv *execenv,
			 struct x86_signal_context *ucontext)
{
	UK_ASSERT(execenv);
	UK_ASSERT(ucontext);

	ucontext->uc_mcontext.gregs[REG_R8] = uk_lcpu_regs_get(execenv->regs,
							       R8);
	ucontext->uc_mcontext.gregs[REG_R9] = uk_lcpu_regs_get(execenv->regs,
							       R9);
	ucontext->uc_mcontext.gregs[REG_R10] = uk_lcpu_regs_get(execenv->regs,
								R10);
	ucontext->uc_mcontext.gregs[REG_R11] = uk_lcpu_regs_get(execenv->regs,
								R11);
	ucontext->uc_mcontext.gregs[REG_R12] = uk_lcpu_regs_get(execenv->regs,
								R12);
	ucontext->uc_mcontext.gregs[REG_R13] = uk_lcpu_regs_get(execenv->regs,
								R13);
	ucontext->uc_mcontext.gregs[REG_R14] = uk_lcpu_regs_get(execenv->regs,
								R14);
	ucontext->uc_mcontext.gregs[REG_R15] = uk_lcpu_regs_get(execenv->regs,
								R15);
	ucontext->uc_mcontext.gregs[REG_RDI] = uk_lcpu_regs_get(execenv->regs,
								RDI);
	ucontext->uc_mcontext.gregs[REG_RSI] = uk_lcpu_regs_get(execenv->regs,
								RSI);
	ucontext->uc_mcontext.gregs[REG_RBP] = uk_lcpu_regs_get(execenv->regs,
								RBP);
	ucontext->uc_mcontext.gregs[REG_RBX] = uk_lcpu_regs_get(execenv->regs,
								RBX);
	ucontext->uc_mcontext.gregs[REG_RDX] = uk_lcpu_regs_get(execenv->regs,
								RDX);
	ucontext->uc_mcontext.gregs[REG_RAX] = uk_lcpu_regs_get(execenv->regs,
								RAX);
	ucontext->uc_mcontext.gregs[REG_RCX] = uk_lcpu_regs_get(execenv->regs,
								RCX);
	ucontext->uc_mcontext.gregs[REG_RSP] = uk_lcpu_regs_get(execenv->regs,
								RSP);
	ucontext->uc_mcontext.gregs[REG_RIP] = uk_lcpu_regs_get(execenv->regs,
								RIP);

	ucontext->uc_mcontext.gregs[REG_EFL] =
		uk_lcpu_regs_get(execenv->regs, RFLAGS);
	ucontext->uc_mcontext.gregs[REG_CSGSFS] =
		uk_lcpu_regs_get(execenv->regs, CS) |
		(uk_lcpu_regs_get(execenv->regs, SS) << 48);
}

static void get_ucontext(struct x86_signal_context *ucontext,
			 struct ukarch_execenv *execenv)
{
	__u64 flags = uk_lcpu_regs_get(execenv->regs, RFLAGS);

	UK_ASSERT(ucontext);
	UK_ASSERT(execenv);

	uk_lcpu_regs_set(execenv->regs, R8,
			 ucontext->uc_mcontext.gregs[REG_R8]);
	uk_lcpu_regs_set(execenv->regs, R9,
			 ucontext->uc_mcontext.gregs[REG_R9]);
	uk_lcpu_regs_set(execenv->regs, R10,
			 ucontext->uc_mcontext.gregs[REG_R10]);
	uk_lcpu_regs_set(execenv->regs, R11,
			 ucontext->uc_mcontext.gregs[REG_R11]);
	uk_lcpu_regs_set(execenv->regs, R12,
			 ucontext->uc_mcontext.gregs[REG_R12]);
	uk_lcpu_regs_set(execenv->regs, R13,
			 ucontext->uc_mcontext.gregs[REG_R13]);
	uk_lcpu_regs_set(execenv->regs, R14,
			 ucontext->uc_mcontext.gregs[REG_R14]);
	uk_lcpu_regs_set(execenv->regs, R15,
			 ucontext->uc_mcontext.gregs[REG_R15]);
	uk_lcpu_regs_set(execenv->regs, RDI,
			 ucontext->uc_mcontext.gregs[REG_RDI]);
	uk_lcpu_regs_set(execenv->regs, RSI,
			 ucontext->uc_mcontext.gregs[REG_RSI]);
	uk_lcpu_regs_set(execenv->regs, RBP,
			 ucontext->uc_mcontext.gregs[REG_RBP]);
	uk_lcpu_regs_set(execenv->regs, RBX,
			 ucontext->uc_mcontext.gregs[REG_RBX]);
	uk_lcpu_regs_set(execenv->regs, RDX,
			 ucontext->uc_mcontext.gregs[REG_RDX]);
	uk_lcpu_regs_set(execenv->regs, RAX,
			 ucontext->uc_mcontext.gregs[REG_RAX]);
	uk_lcpu_regs_set(execenv->regs, RCX,
			 ucontext->uc_mcontext.gregs[REG_RCX]);
	uk_lcpu_regs_set(execenv->regs, RSP,
			 ucontext->uc_mcontext.gregs[REG_RSP]);
	uk_lcpu_regs_set(execenv->regs, RIP,
			 ucontext->uc_mcontext.gregs[REG_RIP]);

	flags = (flags & ~SIGNAL_RFLAGS) |
		(ucontext->uc_mcontext.gregs[REG_EFL] & SIGNAL_RFLAGS);
	uk_lcpu_regs_set(execenv->regs, RFLAGS, flags);
}

void pprocess_signal_return(void);

void pprocess_signal_arch_prepare(struct uk_signal *signal,
				  const struct kern_sigaction *action,
				  struct ukarch_execenv *execenv,
				  const uk_sigset_t *saved_mask)
{
	struct pprocess_signal_stack stack;
	__uptr sp = uk_lcpu_regs_get(execenv->regs, SP);
	struct x86_signal_frame storage = { 0 };
	struct x86_signal_frame *frame = &storage;
	__sz fp_size = uk_plat_native_ectx_size();
	__u64 features = uk_plat_native_ectx_features();
	void *fp, *buffer;
	bool copied;

	stack = pprocess_signal_stack_prepare(action, sp, 128);
	sp = stack.sp;
	if (sp < fp_size + 4 + sizeof(*frame) + 80)
		pprocess_signal_bad_stack();
	fp = (void *)ALIGN_DOWN(sp - fp_size - 4, 64);
	sp = ALIGN_DOWN((__uptr)fp - sizeof(*frame), 16) - 8;
	if (pprocess_signal_stack_reserve(&stack, sp, (__uptr)fp + fp_size + 4))
		pprocess_signal_bad_stack();
	buffer = uk_memalign(uk_alloc_get_default(), 64, fp_size + 4);
	if (!buffer)
		pprocess_signal_bad_stack();
	frame->restorer = (__uptr)(action->ks_flags & SA_RESTORER ?
		action->ks_restorer : pprocess_signal_return);
	frame->info = signal->siginfo;
	set_ucontext(execenv, &frame->context);
	frame->context.uc_sigmask = *saved_mask;
	frame->context.uc_stack = stack.previous;
	frame->context.uc_flags = features ? 7 : 6;
	frame->context.uc_mcontext.fpregs = fp;
	frame->context.uc_mcontext.gregs[REG_OLDMASK] = *saved_mask;
	if (signal->siginfo.si_code > 0 &&
	    (signal->siginfo.si_signo == SIGBUS ||
	     signal->siginfo.si_signo == SIGSEGV))
		frame->context.uc_mcontext.gregs[REG_CR2] =
			(__uptr)signal->siginfo.si_addr;
	uk_plat_native_ectx_export((const void *)execenv->ectx, buffer);
	if (features) {
		struct xstate_metadata *metadata = (void *)((char *)buffer + 464);

		*metadata = (struct xstate_metadata){
			.magic = XSTATE_MAGIC, .extended_size = fp_size + 4,
			.features = features, .state_size = fp_size
		};
		*(__u32 *)((char *)buffer + fp_size) = XSTATE_END;
	}
	copied = uk_nofault_memcpy(fp, buffer, fp_size + (features ? 4 : 0), 0) ==
		fp_size + (features ? 4 : 0);
	uk_free(uk_alloc_get_default(), buffer);
	if (!copied || !uk_nofault_try_write(storage, (struct x86_signal_frame *)sp))
		pprocess_signal_bad_stack();
	frame = (void *)sp;
	uk_lcpu_regs_set(execenv->regs, RIP, (__uptr)action->ks_handler);
	uk_lcpu_regs_set(execenv->regs, RSP, sp);
	uk_lcpu_regs_set(execenv->regs, RDI, signal->siginfo.si_signo);
	uk_lcpu_regs_set(execenv->regs, RSI, (__uptr)&frame->info);
	uk_lcpu_regs_set(execenv->regs, RDX, (__uptr)&frame->context);
	uk_lcpu_regs_set(execenv->regs, RAX, 0);
	uk_lcpu_regs_set(execenv->regs, RFLAGS,
			 uk_lcpu_regs_get(execenv->regs, RFLAGS) & ~0x10500UL);
}

static int restore_fp(struct x86_signal_context *context,
		      struct ukarch_execenv *execenv)
{
	const void *fp = context->uc_mcontext.fpregs;
	struct xstate_metadata metadata;
	__sz size = 512;
	__u64 features = 3;
	__u32 end;
	void *buffer;
	int rc = -EFAULT;

	if (!fp)
		return uk_plat_native_ectx_import((void *)execenv->ectx,
					 NULL, 0, 0);
	if ((__uptr)fp > __PTR_MAX - 512 ||
	    !uk_nofault_try_read(metadata,
		(const struct xstate_metadata *)((const char *)fp + 464)))
		return -EFAULT;
	if (metadata.magic == XSTATE_MAGIC) {
		size = metadata.state_size;
		features = metadata.features;
		if (!IS_ALIGNED((__uptr)fp, 64) || size < 576 ||
		    size > uk_plat_native_ectx_size() ||
		    metadata.extended_size != size + 4 ||
		    (__uptr)fp > __PTR_MAX - size - 4 ||
		    !uk_nofault_try_read(end,
				(const __u32 *)((const char *)fp + size)) ||
		    end != XSTATE_END)
			return -EINVAL;
	}
	buffer = uk_memalign(uk_alloc_get_default(), 64, size);
	if (!buffer)
		return -ENOMEM;
	if (uk_nofault_memcpy(buffer, fp, size, 0) == size)
		rc = uk_plat_native_ectx_import((void *)execenv->ectx,
						buffer, size, features);
	uk_free(uk_alloc_get_default(), buffer);
	return rc;
}

UK_LLSYSCALL_R_E_DEFINE(long, rt_sigreturn)
{
	struct x86_signal_frame storage;
	struct x86_signal_frame *frame = &storage;
	__uptr sp = uk_lcpu_regs_get(execenv->regs, SP);

	if (sp < 8 ||
	    !uk_nofault_try_read(storage, (const struct x86_signal_frame *)(sp - 8)) ||
	    restore_fp(&frame->context, execenv))
		pprocess_signal_bad_stack();
	get_ucontext(&frame->context, execenv);
	pprocess_signal_restore_mask(frame->context.uc_sigmask);
	pprocess_signal_restore_altstack(&frame->context.uc_stack,
					 uk_lcpu_regs_get(execenv->regs, SP));
	return uk_lcpu_regs_get(execenv->regs, RAX);
}
