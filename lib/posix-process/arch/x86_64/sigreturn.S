/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 Bloudraad. */

#include <uk/asm.h>
#include <uk/bits/syscall_nrs.h>

ENTRY(pprocess_signal_return)
	movq $SYS_rt_sigreturn, %rax
	syscall
	ud2
END(pprocess_signal_return)
