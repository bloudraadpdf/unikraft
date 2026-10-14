/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/asm.h>
#include <uk/lcpu.h>
#include <uk/arch/ctx.h>

#include "../../signal/signal.h"

/* Switch to application's context and call signal handler.
 *
 * Before executing the handler, align the auxsp and update
 * auxspcb->current_frame so that any kernel entry that happens
 * while the signal handler executes, starts off a clean state.
 *
 * @param handler_ctx    ptr to struct pprocess_signal_handler_ctx (x0)
 * @param execenv        ptr to application's execenv (x1)
 */
ENTRY(pprocess_signal_jmp_handler)
	stp	x29, x30, [sp, #-16]!

	stp	x19, x20, [sp, #-16]!
	stp	x21, x22, [sp, #-16]!
	stp	x23, x24, [sp, #-16]!
	stp	x25, x26, [sp, #-16]!
	stp	x27, x28, [sp, #-16]!
	stp	d8, d9, [sp, #-16]!
	stp	d10, d11, [sp, #-16]!
	stp	d12, d13, [sp, #-16]!
	stp	d14, d15, [sp, #-16]!
	mov	x29, sp /* save pre-aligned sp */

	mov	x19, x0 /* handler_ctx */
	mov	x20, x1 /* execenv */

	/*───────────────────────────────────────────────────────────────────*
	 * Save uk sysctx                                                    *
	 *───────────────────────────────────────────────────────────────────*/
	sub sp, sp, #UK_LCPU_SYSCTX_SIZE  /* reserve space for ukarch sysctx */
	mov x12, sp
	and x12, x12, #~0xf
	mov sp, x12                         /* make sure we're aligned */

	mov x0, sp                                /* prepare args */
	bl	UK_LCPU_SYSCTX_STORE_FNSYM	/* save uk sysctx */

	/*───────────────────────────────────────────────────────────────────*
	 * Align auxspcb->curr_fp.
	 *───────────────────────────────────────────────────────────────────*/
	uk_pcpuvar_arm64_ldr	x10, UK_LCPU_AUXSP_SYM, x11 /* get auxspcb */
	add			x10, x10, #(UKARCH_AUXSPCB_OFFSETOF_CURR_FP - UKARCH_AUXSPCB_SIZE)
	ldr			x11, [x10]	/* save curr_fp and its addr */
	stp			x11, x10, [sp, #-16]!

	/* Save uk sp *before* we align auxsp for nested calls.
	 * We will be restoring the saved value upon handler return.
	 */
	mov	x21, sp

	/* Align auxsp */
	mov x12, sp
	mov x13, #(UKARCH_AUXSP_ALIGN - 1)
	bic x11, x12, x13
	str	x11, [x10]

	/*───────────────────────────────────────────────────────────────────*
	 * Prepare the application context                                *
	 *───────────────────────────────────────────────────────────────────*/
	mov	x0, x19
	mov	x1, x20

	/*───────────────────────────────────────────────────────────────────*
	 * Call signal handler                                               *
	 *───────────────────────────────────────────────────────────────────*/
	mov	x2, x21
	mov	x3, x29
	adr	x4, 1f
	b	pprocess_signal_handler_enter
1:
	mov	sp, x21			/* switch back to uk sp */

	/*───────────────────────────────────────────────────────────────────*
	 * Restore auxspcb->curr_fp                                          *
	 *───────────────────────────────────────────────────────────────────*/
	ldp	x11, x10, [sp], #16	/* pop auxpcb->curr_fp and &auxpcb->curr_fp */
	str	x11, [x10]		/* restore old value */

	/*───────────────────────────────────────────────────────────────────*
	 * Restore uk sysctx                                                 *
	 *───────────────────────────────────────────────────────────────────*/
	mov	x0, sp				/* prepare args */
	bl	UK_LCPU_SYSCTX_LOAD_FNSYM	/* restore saved sysctx */

	mov	sp, x29				/* restore pre-aligned sp */
	ldp	d14, d15, [sp], #16
	ldp	d12, d13, [sp], #16
	ldp	d10, d11, [sp], #16
	ldp	d8, d9, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16
	ldp	x29, x30, [sp], #16
	ret
