/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/alloc.h>
#include <uk/process.h>
#include <uk/process/events.h>
#if CONFIG_LIBPOSIX_VFS
#include <uk/vfs-path.h>
#endif
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif
#include "process.h"
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SHM
#include "sysv.h"
#endif

#if CONFIG_LIBUKVMEM
void pprocess_vm_unmap_owned(struct posix_vm_context *state,
			     const struct uk_vma_ops *ops)
{
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	__vaddr_t start;
	__sz length;
	int rc;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&vas->mutation_lock);
	for (;;) {
		for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma))
			if (vma->owner == state && (!ops || vma->ops == ops))
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
	state->users = 1;
	state->references = 1;
	state->view.context = state;
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
		if (!process->vm_context)
			return -ENOMEM;
#if CONFIG_LIBPOSIX_PROCESS_BRK && CONFIG_LIBUKVMEM
		pprocess_brk_commit(process->vm_context);
#endif
		return 0;
	}
	uk_mutex_lock(&pprocess_mutex);
	process->vm_context = parent->vm_context;
	UK_ASSERT(process->vm_context);
	process->vm_context->users++;
	process->vm_context->references++;
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
}

void uk_posix_vm_reference_put(struct posix_vm_context *state)
{
	bool release;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(state->references > state->users);
	release = !--state->references;
	UK_ASSERT(!release || !state->users);
	uk_mutex_unlock(&pprocess_mutex);
	if (release)
		uk_free(state->allocator, state);
}

void pprocess_vm_retire_locked(struct posix_vm_context *state,
			       pid_t pid __maybe_unused,
			       struct pprocess_retired *retired)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (!state)
		return;
	UK_ASSERT(state->users && state->references >= state->users);
	if (--state->users) {
		state->references--;
		return;
	}
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SHM
	pprocess_shm_retire_locked(state, pid);
#endif
	state->retired_next = retired->vms;
	retired->vms = state;
}

void pprocess_vm_destroy_retired(struct posix_vm_context *state)
{
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	UK_ASSERT(!state->users);
#if CONFIG_LIBUKVMEM
	pprocess_vm_unmap_owned(state, NULL);
#endif
#if CONFIG_LIBPOSIX_PROCESS_BRK && !CONFIG_LIBUKVMEM
	pprocess_brk_free(state);
#endif
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	uk_free(state->allocator, state->auxv);
	state->auxv = NULL;
#if CONFIG_LIBPOSIX_VFS
	uk_posix_vfs_path_release(state->executable);
	state->executable = NULL;
#endif
#endif
	uk_posix_vm_reference_put(state);
}

void pprocess_vm_release_user(struct posix_vm_context *state, pid_t pid)
{
	struct pprocess_retired retired = {0};

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_retire_locked(state, pid, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
}

void pprocess_vm_replace(struct uk_thread *thread,
			 struct posix_vm_context *state)
{
	struct pprocess_retired retired = {0};
	struct posix_process *process;
	struct posix_thread *target;

	UK_ASSERT(state);
	pprocess_thread_cleanup(thread);
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
#if CONFIG_LIBPOSIX_PROCESS_BRK && CONFIG_LIBUKVMEM
	pprocess_brk_commit(state);
#endif
	uk_arch_mb();
	uk_mutex_lock(&pprocess_mutex);
	target = uk_thread_uktls_var(thread, pthread_self);
	UK_ASSERT(target && target->process);
	process = target->process;
	pprocess_vm_retire_locked(process->vm_context, process->pid, &retired);
	process->vm_context = state;
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(uk_thread_uktls_var(thread, pthread_self) == target);
	target->cleanup_started = false;
	uk_mutex_unlock(&pprocess_mutex);
}

#if CONFIG_LIBPOSIX_PROCESS_EXECVE && CONFIG_LIBPOSIX_VFS
struct uk_vfs_path *
uk_posix_process_task_executable(struct posix_process *process,
				 struct uk_posix_task *task)
{
	struct uk_vfs_path *path = NULL;

	uk_mutex_lock(&pprocess_mutex);
	if (pprocess_task_resolve_locked(process, task) &&
	    process->vm_context && process->vm_context->users)
		path = process->vm_context->executable;
	if (path)
		uk_posix_vfs_path_acquire(path);
	uk_mutex_unlock(&pprocess_mutex);
	return path ? path : ERR2PTR(-ENOENT);
}
#endif

#if CONFIG_LIBUKVMEM && CONFIG_LIBPOSIX_VFS
struct uk_posix_vm_view *
uk_posix_process_task_vm_view_get(struct posix_process *process,
				  struct uk_posix_task *task)
{
	struct uk_posix_vm_view *view = NULL;
	struct posix_vm_context *state;

	uk_mutex_lock(&pprocess_mutex);
	if (!uk_posix_process_task_attached(process, task)) {
		view = ERR2PTR(-ESRCH);
	} else if (pprocess_task_resolve_locked(process, task)) {
		state = process->vm_context;
		if (state && state->users) {
			state->references++;
			view = &state->view;
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	return view;
}
#endif

#if CONFIG_LIBUKVMEM
void uk_posix_vm_view_put(struct uk_posix_vm_view *view)
{
	if (view)
		uk_posix_vm_reference_put(view->context);
}
#endif
