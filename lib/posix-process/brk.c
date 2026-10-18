/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <uk/alloc.h>
#include <uk/paging.h>
#include <uk/print.h>
#include <uk/process.h>
#include <uk/syscall.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>
#endif /* CONFIG_LIBUKVMEM */
#include "process.h"

#if CONFIG_LIBUKVMEM
static inline int pprocess_brk_init_zone(struct posix_vm_context *state)
{
	__vaddr_t va = UK_PAGING_VADDR_ANY;
	int rc;

	rc = uk_vma_map(uk_vas_get_active(), &va, PPROCESS_BRK_LEN,
			UK_PAGING_PAGE_ATTR_PROT_NONE, 0, "brk reservation",
			&uk_vma_rsvd_ops, NULL, state);
	if (unlikely(rc))
		return rc;

	state->brk.base = (void *)va;

	return 0;
}
#else /* !CONFIG_LIBUKVMEM */
static inline int pprocess_brk_init_zone(struct posix_vm_context *state)
{
	/* Take a big enough region from the global allocator. */
	state->brk.base = uk_palloc(state->allocator, PPROCESS_BRK_PAGES);
	if (unlikely(!state->brk.base))
		return -ENOMEM;

	return 0;
}
#endif /* !CONFIG_LIBUKVMEM */

int pprocess_brk_init(struct posix_vm_context *state)
{
	int rc;

	rc = pprocess_brk_init_zone(state);
	if (rc)
		return rc;
	return 0;
}

#if CONFIG_LIBUKVMEM
void pprocess_brk_commit(struct posix_vm_context *state)
{
	pprocess_vm_unmap_owned(state, &uk_vma_rsvd_ops);
}
#else
void pprocess_brk_free(struct posix_vm_context *state)
{
	uk_pfree(state->allocator, state->brk.base, PPROCESS_BRK_PAGES);
}
#endif

static void brk_lock(void)
{
#if CONFIG_LIBUKVMEM
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&uk_vas_get_active()->mutation_lock);
#else
	uk_mutex_lock(&pprocess_mutex);
#endif
}

static void brk_unlock(void)
{
#if CONFIG_LIBUKVMEM
	uk_mutex_unlock(&uk_vas_get_active()->mutation_lock);
#else
	uk_mutex_unlock(&pprocess_mutex);
#endif
}

UK_LLSYSCALL_R_DEFINE(void *, brk, void *, addr)
{
	struct posix_process *pprocess;
	__uptr base, cur_brk, addr_arg;

	pprocess = uk_pprocess_current();
	UK_ASSERT(pprocess);

	brk_lock();

	base = (__uptr)pprocess->vm_context->brk.base;
	cur_brk = base + pprocess->vm_context->brk.pos;
	addr_arg = (__uptr)addr;

	if (addr_arg == cur_brk) {
		brk_unlock();
		return addr;
	}

#if CONFIG_LIBUKVMEM
	if (addr_arg < base) {
#else
	if (!IN_RANGE(addr_arg, base, PPROCESS_BRK_LEN)) {
#endif
		brk_unlock();
		return (void *)cur_brk;
	}

#if CONFIG_LIBUKVMEM
	if (pprocess_memory_brk(pprocess->vm_context, addr_arg - base)) {
		brk_unlock();
		return (void *)cur_brk;
	}
#elif CONFIG_LIBPOSIX_PROCESS_BRK_INC_MEMZERO
	if (addr_arg > cur_brk)
		memset((void *)cur_brk, 0, addr_arg - cur_brk);
#endif

	uk_pr_debug("PID %d brk 0x%lx -> 0x%lx\n",
		    pprocess->pid, cur_brk, addr_arg);

	pprocess->vm_context->brk.pos = addr_arg - base;

	brk_unlock();

	return addr;
}

#if UK_LIBC_SYSCALLS
int brk(void *addr)
{
	void *new_brk;

	/**
	 * In practice glibc seems to return success even when addr is below
	 * current brk position. Since we need to be compatible with other
	 * libcs, mimic this behavior.
	 */
	new_brk = (void *)uk_syscall_r_brk((long)addr);
	if (unlikely(new_brk < addr)) {
		errno = ENOMEM;
		return -1;
	}

	return 0;
}

#if CONFIG_LIBNOLIBC
void *sbrk(intptr_t inc)
{
	struct posix_process *pprocess;
	__uptr req_brk, prev_brk;
	void *new_brk;

	pprocess = uk_pprocess_current();
	UK_ASSERT(pprocess);

	brk_lock();

	/* We are increasing or reducing our range */
	prev_brk = (__uptr)pprocess->vm_context->brk.base + pprocess->vm_context->brk.pos;

	/* Check for 0 and overflow/underflow */
	if (!inc) {
		brk_unlock();
		return (void *)prev_brk;
	} else if (inc > 0) {
		if (unlikely(prev_brk + (__uptr)inc < prev_brk))
			goto err;
	} else {
		if (unlikely(prev_brk < -(__uptr)inc))
			goto err;
	}

	req_brk = (__uptr)prev_brk + inc;
	new_brk = (void *)uk_syscall_r_brk((long)req_brk);
	if (unlikely((__uptr)new_brk != req_brk))
		goto err;

	brk_unlock();
	return (void *)prev_brk;

err:
	brk_unlock();
	errno = ENOMEM;
	return (void *)-1;
}
#endif /* CONFIG_LIBNOLIBC */
#endif /* UK_LIBC_SYSCALLS */
