/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/asm.h>
#include <uk/bits/syscall_nrs.h>

ENTRY(pprocess_signal_return)
	mov	x8, #SYS_rt_sigreturn
	svc	#0
	brk	#0
END(pprocess_signal_return)
