/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/arch/limits.h>
#include <uk/process.h>
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>
#include "process.h"

struct memory_pages {
	__sz address;
	__sz data;
};

static void memory_lock(struct uk_vas *vas)
{
	uk_mutex_lock(&pprocess_mutex);
	uk_mutex_lock(&vas->mutation_lock);
}

static void memory_unlock(struct uk_vas *vas)
{
	uk_mutex_unlock(&vas->mutation_lock);
	uk_mutex_unlock(&pprocess_mutex);
}

static struct posix_vm_context *memory_current(void)
{
	struct posix_thread *thread = uk_pthread_current();

	if (!thread)
		return NULL;
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	if (thread->exec_vm)
		return thread->exec_vm;
#endif
	return thread->process->vm_context;
}

static bool memory_is_data(bool private, unsigned long attr)
{
	return private && (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) &&
		!(attr & UK_PAGING_PAGE_ATTR_PROT_EXEC);
}

static __sz memory_overlap(const struct uk_vma *vma,
			   struct posix_vm_context *state,
			   __vaddr_t start, __vaddr_t end)
{
	__vaddr_t left = MAX(vma->start, start);
	__vaddr_t right = MIN(vma->end, end);

	if (vma->owner != state || left >= right)
		return 0;
	return (right - left) >> UK_PAGING_PAGE_SHIFT;
}

static struct memory_pages memory_ranges(struct uk_vas *vas,
					 struct posix_vm_context *state,
					 __vaddr_t start, __vaddr_t end)
{
	struct memory_pages pages = {0};
	const struct uk_vma *vma;
	__sz length;

	for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma)) {
		length = memory_overlap(vma, state, start, end);
		pages.address += length;
		if (memory_is_data(vma->flags & UK_VMA_FLAG_PRIVATE, vma->attr))
			pages.data += length;
	}
	return pages;
}

static struct memory_pages memory_usage(struct uk_vas *vas,
					struct posix_vm_context *state)
{
	struct memory_pages pages = memory_ranges(vas, state, 0, __VADDR_MAX);

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	pages.address += UK_PAGING_PAGE_ALIGN_UP(state->stack_size) >>
		UK_PAGING_PAGE_SHIFT;
#endif
	return pages;
}

static bool memory_fits(__sz used, __sz added, rlim_t limit)
{
	limit >>= UK_PAGING_PAGE_SHIFT;
	return added <= limit && used <= limit - added;
}

static rlim_t memory_data_limit(struct posix_process *process)
{
	struct rlimit limit = process->limits[RLIMIT_DATA];

	return limit.rlim_cur ? limit.rlim_cur : limit.rlim_max;
}

int uk_posix_process_vm_map(struct uk_vas *vas, __vaddr_t *address,
			    __sz length, unsigned long attr, unsigned long flags,
			    const struct uk_vma_ops *ops, void *args)
{
	struct posix_vm_context *state;
	struct posix_process *process;
	struct memory_pages usage, removed = {0};
	__sz pages = length >> UK_PAGING_PAGE_SHIFT;
	int rc;

	memory_lock(vas);
	state = memory_current();
	if (state) {
		process = uk_pprocess_current();
		usage = memory_usage(vas, state);
		if (flags & UK_VMA_MAP_REPLACE) {
			if (*address > __VADDR_MAX - length) {
				rc = -ENOMEM;
				goto out;
			}
			removed = memory_ranges(vas, state, *address,
						*address + length);
		}
		if (!memory_fits(usage.address - removed.address, pages,
				 process->limits[RLIMIT_AS].rlim_cur) ||
		    (memory_is_data(flags & UK_VMA_MAP_PRIVATE, attr) &&
		     !memory_fits(usage.data - removed.data, pages,
				  memory_data_limit(process)))) {
			rc = -ENOMEM;
			goto out;
		}
	}
#if CONFIG_LIBPOSIX_PROCESS_BRK
	if (state && *address == UK_PAGING_VADDR_ANY &&
	    (__vaddr_t)state->brk.base <= __VADDR_MAX - PPROCESS_BRK_LEN) {
		__vaddr_t preferred = (__vaddr_t)state->brk.base + PPROCESS_BRK_LEN;

		rc = uk_vma_map(vas, &preferred, length, attr,
				flags | UK_VMA_MAP_SEARCH, NULL, ops, args, state);
		if (!rc) {
			*address = preferred;
			goto out;
		}
		if (rc != -ENOMEM)
			goto out;
	}
#endif
	rc = uk_vma_map(vas, address, length, attr, flags, NULL, ops, args, state);
out:
	memory_unlock(vas);
	return rc;
}

int uk_posix_process_vm_unmap(struct uk_vas *vas, __vaddr_t address, __sz length)
{
	int rc;

	memory_lock(vas);
	rc = uk_vma_unmap(vas, address, length, 0);
	memory_unlock(vas);
	return rc;
}

int uk_posix_process_vm_protect(struct uk_vas *vas, __vaddr_t address,
				__sz length, unsigned long attr)
{
	struct posix_vm_context *state;
	struct posix_process *process;
	struct memory_pages usage;
	const struct uk_vma *vma;
	__sz pages;
	int rc;

	if (address > __VADDR_MAX - length)
		return -ENOMEM;
	memory_lock(vas);
	state = memory_current();
	if (state) {
		process = uk_pprocess_current();
		usage = memory_usage(vas, state);
		for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma)) {
			pages = memory_overlap(vma, state, address, address + length);
			if (!pages || !(vma->flags & UK_VMA_FLAG_PRIVATE) ||
			    !memory_is_data(true, attr) ||
			    memory_is_data(true, vma->attr))
				continue;
			if (memory_fits(usage.address, pages,
					process->limits[RLIMIT_AS].rlim_cur) &&
			    !memory_fits(usage.data, pages,
					 memory_data_limit(process))) {
				rc = -ENOMEM;
				goto out;
			}
			usage.data += pages;
		}
	}
	rc = uk_vma_set_attr(vas, address, length, attr,
			     UK_VMA_FLAG_STRICT_VMA_CHECK);
out:
	memory_unlock(vas);
	return rc;
}

int uk_posix_process_vm_advise(struct uk_vas *vas, __vaddr_t address,
			       __sz length, unsigned long advice)
{
	int rc;

	memory_lock(vas);
	rc = uk_vma_advise(vas, address, length, advice,
			   UK_VMA_FLAG_STRICT_VMA_CHECK);
	memory_unlock(vas);
	return rc;
}

int uk_posix_process_vm_sync(struct uk_vas *vas, __vaddr_t address, __sz length)
{
	int rc;

	memory_lock(vas);
	rc = uk_vma_sync(vas, address, length);
	memory_unlock(vas);
	return rc;
}

#if CONFIG_LIBPOSIX_PROCESS_BRK
int pprocess_memory_brk(struct posix_vm_context *state, __sz position)
{
	struct uk_vas *vas = uk_vas_get_active();
	struct posix_process *process = uk_pprocess_current();
	struct memory_pages usage;
	const struct uk_vma *vma;
	__vaddr_t address, end;
	__sz old_size, new_size, pages;
	rlim_t data_limit;
	int rc = 0;

	if (position > __SZ_MAX - (UK_PAGING_PAGE_SIZE - 1))
		return -ENOMEM;
	old_size = UK_PAGING_PAGE_ALIGN_UP(state->brk.pos);
	new_size = UK_PAGING_PAGE_ALIGN_UP(position);
	if ((__vaddr_t)state->brk.base > __VADDR_MAX - new_size)
		return -ENOMEM;
	memory_lock(vas);
	data_limit = process->limits[RLIMIT_DATA].rlim_cur;
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	if (state->image_data_size > data_limit) {
		rc = -ENOMEM;
		goto out;
	}
	data_limit -= state->image_data_size;
#endif
	if (position > data_limit) {
		rc = -ENOMEM;
		goto out;
	}
	if (position > state->brk.pos) {
		if (new_size == old_size)
			goto out;
		usage = memory_usage(vas, state);
		pages = (new_size - old_size) >> UK_PAGING_PAGE_SHIFT;
		if (!memory_fits(usage.address, pages,
				 process->limits[RLIMIT_AS].rlim_cur) ||
		    !memory_fits(usage.data, pages,
				 process->limits[RLIMIT_DATA].rlim_cur)) {
			rc = -ENOMEM;
			goto out;
		}
		address = (__vaddr_t)state->brk.base + old_size;
		rc = uk_vma_map(vas, &address, new_size - old_size,
				UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_MAP_PRIVATE,
				"brk", &uk_vma_anon_ops, NULL, state);
	} else if (new_size < old_size) {
		address = (__vaddr_t)state->brk.base + new_size;
		end = (__vaddr_t)state->brk.base + old_size;
		for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma)) {
			if (vma->start >= end)
				break;
			if (vma->end > address && vma->owner != state) {
				rc = -ENOMEM;
				goto out;
			}
		}
		rc = uk_vma_unmap(vas, address, old_size - new_size, 0);
	}
out:
	memory_unlock(vas);
	return rc;
}
#endif

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
int pprocess_stack_trim(struct posix_vm_context *state,
			__vaddr_t *bottom, __vaddr_t top, __vaddr_t sp)
{
#if CONFIG_LIBUKALLOCSTACK_PAGE_GUARDS
	struct uk_vas *vas = uk_vas_get_active();
	rlim_t limit = uk_pprocess_current()->limits[RLIMIT_STACK].rlim_cur;
	__sz used = UK_PAGING_PAGE_ALIGN_UP(top - sp);
	__sz length = MAX(used, 32 * UK_PAGING_PAGE_SIZE);
	int rc;

	limit = UK_PAGING_PAGE_ALIGN_DOWN(limit);
	if (used > limit)
		return -E2BIG;
	length = MIN(length, limit);
	rc = uk_vma_stack_resize(vas, *bottom, top - length);
	if (rc)
		return rc;
	*bottom = top - length;
	state->stack_base = *bottom;
	state->stack_top = top;
	state->stack_size = length;
#else
	state->stack_size = top - *bottom;
#endif
	return 0;
}

int pprocess_memory_stack(__vaddr_t address)
{
	struct uk_vas *vas = uk_vas_get_active();
	struct posix_vm_context *state;
	struct posix_process *process;
	struct memory_pages usage;
	const struct uk_vma *vma;
	__vaddr_t bottom = UK_PAGING_PAGE_ALIGN_DOWN(address);
	__sz length, pages;
	int rc = -EFAULT;

	memory_lock(vas);
	state = memory_current();
	if (!state || !state->stack_base || bottom >= state->stack_top)
		goto out;
	vma = uk_vma_find(vas, state->stack_base);
	if (!vma || vma->ops != &uk_vma_stack_ops ||
	    bottom >= vma->start + UK_VMA_STACK_BOTTOM_GUARD_SIZE)
		goto out;
	length = state->stack_top - bottom;
	process = uk_pprocess_current();
	if (length > process->limits[RLIMIT_STACK].rlim_cur)
		goto out;
	usage = memory_usage(vas, state);
	pages = (length - state->stack_size) >> UK_PAGING_PAGE_SHIFT;
	if (!memory_fits(usage.address, pages,
			 process->limits[RLIMIT_AS].rlim_cur))
		goto out;
	rc = uk_vma_stack_resize(vas, state->stack_base, bottom);
	if (rc)
		goto out;
	state->stack_size = length;
	rc = uk_vma_populate_page(vas, address);
out:
	memory_unlock(vas);
	return rc;
}
#endif
