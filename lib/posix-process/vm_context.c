/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/alloc.h>
#include <uk/process.h>
#include <uk/process/events.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif
#include "process.h"
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SHM
#include "sysv.h"
#endif

#if CONFIG_LIBUKVMEM
const void *uk_posix_process_vm_owner(void)
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

static void pprocess_vm_unmap(struct posix_vm_context *state)
{
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	__vaddr_t start;
	__sz length;
	int rc;

	uk_mutex_lock(&vas->mutation_lock);
	for (;;) {
		for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma))
			if (vma->owner == state)
				break;
		if (!vma)
			break;
		start = vma->start;
		length = vma->end - start;
		rc = uk_vma_unmap(vas, start, length, 0);
		UK_BUGON(rc);
	}
	uk_mutex_unlock(&vas->mutation_lock);
}
#endif

struct posix_vm_context *pprocess_vm_alloc(struct uk_alloc *allocator)
{
	struct posix_vm_context *state = uk_zalloc(allocator, sizeof(*state));

	if (!state)
		return NULL;
	state->allocator = allocator;
	state->references = 1;
#if CONFIG_LIBPOSIX_PROCESS_BRK
	if (pprocess_brk_init(state)) {
		uk_free(allocator, state);
		return NULL;
	}
#endif
	return state;
}

int pprocess_vm_init(struct posix_process *process, struct posix_process *parent)
{
	if (!parent) {
		process->vm_context = pprocess_vm_alloc(process->_a);
		return process->vm_context ? 0 : -ENOMEM;
	}
	uk_mutex_lock(&pprocess_mutex);
	process->vm_context = parent->vm_context;
	UK_ASSERT(process->vm_context);
	process->vm_context->references++;
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
}

void pprocess_vm_put(struct posix_vm_context *state, pid_t pid __maybe_unused)
{
	if (!state)
		return;
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(state->references);
	if (!--state->references) {
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SHM
		pprocess_shm_release(state, pid);
#endif
#if CONFIG_LIBUKVMEM
		pprocess_vm_unmap(state);
#endif
#if CONFIG_LIBPOSIX_PROCESS_BRK
		pprocess_brk_free(state);
#endif
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
		uk_free(state->allocator, state->auxv);
#endif
		uk_free(state->allocator, state);
	}
	uk_mutex_unlock(&pprocess_mutex);
}

void pprocess_vm_replace(struct posix_process *process,
			struct posix_vm_context *state)
{
	UK_ASSERT(state);
	uk_arch_mb();
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_put(process->vm_context, process->pid);
	process->vm_context = state;
	uk_mutex_unlock(&pprocess_mutex);
}

static int pprocess_vm_exit(void *arg)
{
	struct posix_process_ppexit_event_data *event = arg;
	struct posix_process *process;

	uk_mutex_lock(&pprocess_mutex);
	process = pid2pprocess(event->pid);
	pprocess_vm_put(process->vm_context, process->pid);
	process->vm_context = NULL;
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
}

POSIX_PROCESS_PPEXIT_HANDLER(pprocess_vm_exit);
