/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Pierre Oliver <pierre.olivier@manchester.ac.uk>
 *          Stefan Lankes <slankes@eonerc.rwth-aachen.de>
 *          Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Copyright (c) 2019, Virginia Polytechnic Institute and State University.
 *                     All rights reserved.
 * Copyright (c) 2019, RWTH Aachen University, Germany.
 *.                    All rights reserved.
 * Copyright (c) 2019, NEC Laboratories Europe GmbH,
 *                     NEC Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * With the approval of the original authors, some code in this file was
 * derived and adopted from:
 * HermiTux
 * https://github.com/ssrg-vt/hermitux, commit 8780d335
 * File: apps/hermitux-light/hermitux-light.c
 * The authors of the original implementation are attributed in the license
 * header of this file.
 */

#include <limits.h>
#include <string.h>
#include <uk/assert.h>
#include <uk/binfmt.h>
#include <uk/arch/limits.h>
#include <uk/print.h>
#include <uk/essentials.h>
#if CONFIG_ARCH_ARM_64
#include <bits/hwcap.h>
#include <uk/arch/arm64.h>
#include <uk/arch/util.h>
#endif /* CONFIG_ARCH_ARM_64 */

#include "elf_prog.h"

/* Fields for auxiliary vector
 * (https://lwn.net/Articles/519085/)
 */
#define AT_NULL			0
#define AT_IGNORE		1
#define AT_EXECFD		2
#define AT_PHDR			3
#define AT_PHENT		4
#define AT_PHNUM		5
#define AT_PAGESZ		6
#define AT_BASE			7
#define AT_FLAGS		8
#define AT_ENTRY		9
#define AT_NOTELF		10
#define AT_UID			11
#define AT_EUID			12
#define AT_GID			13
#define AT_EGID			14
#define AT_PLATFORM		15
#define AT_HWCAP		16
#define AT_CLKTCK		17
#define AT_DCACHEBSIZE		19
#define AT_ICACHEBSIZE		20
#define AT_UCACHEBSIZE		21
#define AT_SECURE		23
#define AT_RANDOM		25
#define AT_RSEQ_FEATURE_SIZE	27
#define AT_RSEQ_ALIGN		28
#define AT_EXECFN		31
#define AT_SYSINFO_EHDR		33
#define AT_SYSINFO		32

struct auxv_entry {
	long key;
	long val;
};

#if CONFIG_APPELFLOADER_VDSO
extern char *vdso_image_addr;
#endif /* CONFIG_APPELFLOADER_VDSO */

#if CONFIG_ARCH_X86_64
#define UK_AUXV_PLATFORM	"x86_64"
#elif CONFIG_ARCH_ARM_64
#define UK_AUXV_PLATFORM	"aarch64"
#else
#error "Unsupported architecture"
#endif

#if CONFIG_ARCH_ARM_64
#define ID_FIELD(val, reg, field)					\
	(((val) >> UK_ARCH_ARM64_##reg##_##field##_SHIFT) &		\
	 UK_ARCH_ARM64_##reg##_##field##_MASK)

/* Signed 4-bit field: 0xf reads as -1 ("not implemented") */
#define ID_SFIELD(val, reg, field)					\
	((int)((__s64)((val) <<						\
	 (60 - UK_ARCH_ARM64_##reg##_##field##_SHIFT)) >> 60))

/*
 * AT_HWCAP as Linux reports it on arm64 (uapi/asm/hwcap.h), from the ID
 * registers: userspace learns the CPU's features from it, and some
 * programs refuse to run without it (.NET needs ASIMD).  Left out are
 * features whose state the kernel would have to manage for the program
 * (SVE, pointer authentication keys, the timer event stream) and CPUID:
 * at EL1 the ID registers read unfiltered, so they would advertise
 * features Unikraft leaves disabled (SME, say).  FP and Advanced SIMD
 * instructions, the crypto ones included, need CONFIG_FPSIMD.
 */
static __u64 elf_arm64_hwcap(void)
{
	__u64 pfr0 = UK_ARCH_ARM64_SYSREG_READ64(id_aa64pfr0_el1);
	__u64 pfr1 = UK_ARCH_ARM64_SYSREG_READ64(id_aa64pfr1_el1);
	__u64 isar0 = UK_ARCH_ARM64_SYSREG_READ64(id_aa64isar0_el1);
	__u64 isar1 = UK_ARCH_ARM64_SYSREG_READ64(id_aa64isar1_el1);
	__u64 mmfr2 =
		UK_ARCH_ARM64_SYSREG_READ64(UK_ARCH_ARM64_ID_AA64MMFR2_EL1);
	__u64 hwcap = 0;

#if CONFIG_FPSIMD
	if (ID_SFIELD(pfr0, ID_AA64PFR0_EL1, FP) >= 0)
		hwcap |= HWCAP_FP;
	if (ID_SFIELD(pfr0, ID_AA64PFR0_EL1, FP) >= 1)
		hwcap |= HWCAP_FPHP;
	if (ID_SFIELD(pfr0, ID_AA64PFR0_EL1, ADVSIMD) >= 0)
		hwcap |= HWCAP_ASIMD;
	if (ID_SFIELD(pfr0, ID_AA64PFR0_EL1, ADVSIMD) >= 1)
		hwcap |= HWCAP_ASIMDHP;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, AES) >= 1)
		hwcap |= HWCAP_AES;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, AES) >= 2)
		hwcap |= HWCAP_PMULL;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SHA1) >= 1)
		hwcap |= HWCAP_SHA1;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SHA2) >= 1)
		hwcap |= HWCAP_SHA2;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SHA2) >= 2)
		hwcap |= HWCAP_SHA512;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, RDM) >= 1)
		hwcap |= HWCAP_ASIMDRDM;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SHA3) >= 1)
		hwcap |= HWCAP_SHA3;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SM3) >= 1)
		hwcap |= HWCAP_SM3;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, SM4) >= 1)
		hwcap |= HWCAP_SM4;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, DP) >= 1)
		hwcap |= HWCAP_ASIMDDP;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, FHM) >= 1)
		hwcap |= HWCAP_ASIMDFHM;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, JSCVT) >= 1)
		hwcap |= HWCAP_JSCVT;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, FCMA) >= 1)
		hwcap |= HWCAP_FCMA;
#endif /* CONFIG_FPSIMD */
	if (ID_FIELD(pfr0, ID_AA64PFR0_EL1, DIT) >= 1)
		hwcap |= HWCAP_DIT;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, CRC32) >= 1)
		hwcap |= HWCAP_CRC32;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, ATOMIC) >= 2)
		hwcap |= HWCAP_ATOMICS;
	if (ID_FIELD(isar0, ID_AA64ISAR0_EL1, TS) >= 1)
		hwcap |= HWCAP_FLAGM;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, DPB) >= 1)
		hwcap |= HWCAP_DCPOP;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, LRCPC) >= 1)
		hwcap |= HWCAP_LRCPC;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, LRCPC) >= 2)
		hwcap |= HWCAP_ILRCPC;
	if (ID_FIELD(isar1, ID_AA64ISAR1_EL1, SB) >= 1)
		hwcap |= HWCAP_SB;
	if (ID_FIELD(mmfr2, ID_AA64MMFR2_EL1, AT) >= 1)
		hwcap |= HWCAP_USCAT;
	if (ID_FIELD(pfr1, ID_AA64PFR1_EL1, SSBS) >= 2)
		hwcap |= HWCAP_SSBS;
	return hwcap;
}
#define UK_AUXV_HWCAP		elf_arm64_hwcap()
#else /* !CONFIG_ARCH_ARM_64 */
#define UK_AUXV_HWCAP		0x0
#endif /* !CONFIG_ARCH_ARM_64 */


struct elf_arg_count {
	int argc;
	int envc;
	__sz strings;
};

static int elf_count_string(const char *string, __sz *remaining, __sz *size)
{
	__sz length = strnlen(string, UK_BINFMT_ARG_STRLEN);

	if (length == UK_BINFMT_ARG_STRLEN ||
	    length + 1 + sizeof(__uptr) > *remaining)
		return -E2BIG;
	*remaining -= length + 1 + sizeof(__uptr);
	*size += length + 1;
	return 0;
}

static int elf_arg_env_count(struct elf_arg_count *count, const char *argv0,
			     const char **argv, const char **envp,
			     __sz stack_limit)
{
	const char **vectors[] = { argv, envp };
	int *counts[] = { &count->argc, &count->envc };
	__sz remaining = uk_binfmt_arg_limit(stack_limit);
	int rc;

	memset(count, 0, sizeof(*count));
	if (argv0) {
		rc = elf_count_string(argv0, &remaining, &count->strings);
		if (rc)
			return rc;
		++count->argc;
	}
	for (__sz i = 0; i < ARRAY_SIZE(vectors); ++i) {
		const char **vector = vectors[i];

		for (; vector && *vector; ++vector) {
			rc = elf_count_string(*vector, &remaining,
					      &count->strings);
			if (rc)
				return rc;
			++*counts[i];
		}
	}
	return 0;
}

static int infoblk_push(__uptr *sp, __uptr bottom, const void *buffer,
			__sz size, __uptr *pointer)
{
	if (size > *sp - bottom)
		return -E2BIG;
	*sp -= size;
	memcpy((void *)*sp, buffer, size);
	*pointer = *sp;
	return 0;
}

static int infoblk_push_string(__uptr *sp, __uptr bottom, const char *string,
			       __uptr *pointer)
{
	__sz length = strnlen(string, UK_BINFMT_ARG_STRLEN);

	if (length == UK_BINFMT_ARG_STRLEN)
		return -E2BIG;
	return infoblk_push(sp, bottom, string, length + 1, pointer);
}

int elf_ctx_init(struct ukarch_ctx *ctx, struct elf_prog *prog,
		 const char *argv0, const char *argv[],
		 const char *environ[], __uptr stack_bottom,
		 __sz stack_limit, __u64 rand[2], const __uptr **auxv_start)
{
	struct elf_arg_count count;
	__uptr sp = ctx->sp;
	__uptr vector_sp, info_bottom;
	__uptr *argvec, *envvec, *auxvec;
	__sz required, vectors, path_size;
	int i, rc, prefix;

	*auxv_start = NULL;
	if (!argv0 && (!argv || !argv[0]))
		argv0 = "";
	prefix = !!argv0;
	rc = elf_arg_env_count(&count, argv0, argv, environ, stack_limit);
	if (rc)
		return rc;

	UK_ASSERT(prog);
	UK_ASSERT(count.argc >= 1);

	uk_pr_debug("%s: image:          0x%"PRIx64" - 0x%"PRIx64"\n",
		    prog->name, (uint64_t) prog->vabase,
		    (uint64_t) prog->vabase + prog->valen);
	uk_pr_debug("%s: start:          0x%"PRIx64"\n", prog->name,
		    (uint64_t) prog->start);
	uk_pr_debug("%s: entry:          0x%"PRIx64"\n", prog->name,
		    (uint64_t) prog->entry);
	uk_pr_debug("%s: phdr.off:       0x%"PRIx64"\n", prog->name,
		    (uint64_t) prog->phdr.off);
	uk_pr_debug("%s: phdr.num:       %"PRIu64"\n",   prog->name,
		    (uint64_t) prog->phdr.num);
	uk_pr_debug("%s: phdr.entsize:   0x%"PRIx64"\n", prog->name,
		    (uint64_t) prog->phdr.entsize);
	if (prog->interp.prog) {
		uk_pr_debug("%s: interp:         0x%"PRIx64" - 0x%"PRIx64"\n",
			    prog->name, (uint64_t) prog->interp.prog->vabase,
			    (uint64_t) prog->interp.prog->vabase +
			    prog->interp.prog->valen);
		uk_pr_debug("%s: interp.start:   0x%"PRIx64"\n", prog->name,
			    (uint64_t) prog->interp.prog->start);
		uk_pr_debug("%s: interp.entry:   0x%"PRIx64"\n", prog->name,
			    (uint64_t) prog->interp.prog->entry);
	}

	/* list of all auxiliary vector entries whose pointer values need
	 * to point to information block areas
	 */
	struct auxv_entry infoblk_auxv[] = {
		{ AT_PLATFORM, (__uptr)UK_AUXV_PLATFORM	},
		{ AT_EXECFN, (__uptr)(prog->path ? prog->path : prog->name) },
		{ AT_RANDOM, (__uptr)rand },
	};

	/* list of all auxiliary vector entries whose pointer values need
	 * not be in the information block
	 */
	struct auxv_entry auxv[] = {
		{ AT_NOTELF, 0x0 },
		{ AT_UCACHEBSIZE, 0x0 },
		{ AT_ICACHEBSIZE, 0x0 },
		{ AT_DCACHEBSIZE, 0x0 },
		{ AT_SECURE, 0x0 },
		{ AT_EGID, 0x0 },
		{ AT_GID, 0x0 },
		{ AT_EUID, 0x0 },
		{ AT_UID, 0x0 },
		{ AT_ENTRY, prog->entry },
		{ AT_FLAGS, 0x0 },
		{ AT_CLKTCK, 0x64 }, /* Mimic Linux */
		{ AT_HWCAP, UK_AUXV_HWCAP },
		{ AT_PAGESZ, 4096 },
		/* base addr of interpreter */
		{ AT_BASE, prog->interp.prog ?
			   prog->interp.prog->start : 0x0 },
		{ AT_PHENT, prog->phdr.entsize },
		{ AT_PHNUM, prog->phdr.num },
		{ AT_PHDR, prog->load_bias + prog->phdr.off },
#if CONFIG_APPELFLOADER_VDSO
		/* TODO: This must also be pushed and copied
		 * or mapped to information block. Move it to infoblk_auxv
		 * ASAP.
		 */
		{ AT_SYSINFO_EHDR, (uintptr_t)vdso_image_addr },
#endif /* CONFIG_APPELFLOADER_VDSO */
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		{ AT_RSEQ_FEATURE_SIZE, 20 },
		{ AT_RSEQ_ALIGN, 32 },
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
		{ AT_IGNORE, 0x0 }
	};
	struct auxv_entry auxv_null = { AT_NULL, 0x0 };

	path_size = strlen((const char *)infoblk_auxv[1].val);
	if (__builtin_add_overflow(path_size, (__sz)1, &path_size) ||
	    __builtin_add_overflow(count.strings, path_size, &required) ||
	    __builtin_add_overflow(required,
				   (__sz)(sizeof(UK_AUXV_PLATFORM) + sizeof(__u64[2])),
				   &required) ||
	    __builtin_mul_overflow((__sz)count.argc + count.envc + 3,
				   (__sz)sizeof(__uptr), &vectors) ||
	    __builtin_add_overflow(vectors,
				   (__sz)(sizeof(auxv) + sizeof(infoblk_auxv) +
					  sizeof(auxv_null)), &vectors) ||
	    __builtin_add_overflow(required, vectors, &required) ||
	    sp < stack_bottom || required > sp - stack_bottom)
		return -E2BIG;
	vector_sp = ALIGN_DOWN(sp - required, UKARCH_SP_ALIGN);
	if (vector_sp < stack_bottom)
		return -E2BIG;

	argvec = (__uptr *)vector_sp + 1;
	envvec = argvec + count.argc + 1;
	auxvec = envvec + count.envc + 1;
	info_bottom = vector_sp + vectors;
	for (i = (int)ARRAY_SIZE(infoblk_auxv) - 1; i >= 0; --i) {
		__uptr pointer;

		if (infoblk_auxv[i].key == AT_RANDOM)
			rc = infoblk_push(&sp, info_bottom,
					  (void *)infoblk_auxv[i].val,
					  sizeof(__u64[2]), &pointer);
		else
			rc = infoblk_push_string(&sp, info_bottom,
						 (char *)infoblk_auxv[i].val,
						 &pointer);
		if (rc)
			return rc;
		infoblk_auxv[i].val = pointer;
	}
	for (i = count.envc - 1; i >= 0; --i) {
		rc = infoblk_push_string(&sp, info_bottom, environ[i], &envvec[i]);
		if (rc)
			return rc;
	}
	envvec[count.envc] = 0;
	for (i = count.argc - prefix - 1; i >= 0; --i) {
		rc = infoblk_push_string(&sp, info_bottom, argv[i],
					 &argvec[i + prefix]);
		if (rc)
			return rc;
	}
	if (argv0) {
		rc = infoblk_push_string(&sp, info_bottom, argv0, &argvec[0]);
		if (rc)
			return rc;
	}
	argvec[count.argc] = 0;
	memcpy(auxvec, auxv, sizeof(auxv));
	memcpy((char *)auxvec + sizeof(auxv), infoblk_auxv,
	       sizeof(infoblk_auxv));
	memcpy((char *)auxvec + sizeof(auxv) + sizeof(infoblk_auxv),
	       &auxv_null, sizeof(auxv_null));
	*(long *)vector_sp = count.argc;
	ctx->sp = vector_sp;

	/* ctx will enter the entry point with cleared registers. */
	if (prog->interp.required) {
		struct elf_prog *interp = prog->interp.prog;

		UK_ASSERT(prog->interp.prog);

		/* dynamically linked executable, jump into loader instead */
		ukarch_ctx_init(ctx, ctx->sp, 0x0, interp->entry);
	} else {
		/* statically linked executable */
		ukarch_ctx_init(ctx, ctx->sp, 0x0, prog->entry);
	}
	*auxv_start = auxvec;
	return 0;
}
