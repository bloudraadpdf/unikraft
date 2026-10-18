/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/binfmt.h>
#include <uk/config.h>
#include <uk/event.h>
#include <uk/essentials.h>
#include <uk/plat/config.h>
#include <uk/process.h>
#include <uk/syscall.h>
#include <uk/thread.h>
#include <uk/sched.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <uk/nofault.h>
#if CONFIG_LIBPOSIX_VFS
#include <uk/vfs-path.h>
#include <uk/posix-fd.h>
#endif

#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#endif

#include "process.h"
#include "execve-args.h"
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#endif

void execve_arch_execenv_init(struct ukarch_execenv *execenv_new,
			      struct ukarch_execenv *execenv,
			      __uptr ip, __uptr sp);

static int execve_auxv_init(struct posix_vm_context *state,
			    const __uptr *auxv, const __uptr *stack_end)
{
	const __uptr *auxv_end;

	if (!auxv)
		return 0;
	for (auxv_end = auxv; stack_end - auxv_end >= 2; auxv_end += 2)
		if (!auxv_end[0])
			break;
	if (stack_end - auxv_end < 2)
		return -ENOEXEC;
	state->auxv_size = (auxv_end + 2 - auxv) * sizeof(*auxv);
	state->auxv = uk_malloc(state->allocator, state->auxv_size);
	if (!state->auxv)
		return -ENOMEM;
	memcpy(state->auxv, auxv, state->auxv_size);
	return 0;
}

/* arg0: execenv_new, arg1: stack_old */
static void __noreturn execve_ctx_switch(long arg0, long arg1)
{
	struct ukarch_execenv *execenv_new;
	struct posix_thread *pthread;
	struct uk_thread *this_thread;
	void *stack_old;

	execenv_new = (struct ukarch_execenv *)arg0;
	stack_old = (void *)arg1;

	this_thread = uk_thread_current();
	UK_ASSERT(this_thread);

	pthread = uk_pthread_current();
	UK_ASSERT(pthread);

#if CONFIG_LIBPOSIX_FDTAB
	uk_fdtab_execve_commit(this_thread);
#endif
	pprocess_complete_vfork(pthread);
	if (stack_old)
		uk_free(this_thread->_mem.stack_a, stack_old);

#if CONFIG_ARCH_ARM_64
	pprocess_execenv_load((long)execenv_new);
#else
	ukarch_execenv_load((long)execenv_new);
#endif
}

static void execve_source_close(struct uk_binfmt_source *source)
{
#if CONFIG_LIBPOSIX_VFS
	if (source->file) {
		uk_ofile_release(source->file);
		source->file = NULL;
	}
	uk_posix_vfs_path_release(source->path);
	source->path = NULL;
#else
	if (source->fd >= 0) {
		close(source->fd);
		source->fd = -1;
	}
#endif
}

UK_SYSCALL_R_E_DEFINE(int, execve, const char *, pathname,
		      char *const *, argv,
		      char *const *, envp)
{
	struct posix_process_execve_event_data event_data;
	struct uk_binfmt_loader_args loader_args = {0};
	struct uk_binfmt_source source = {
#if CONFIG_LIBPOSIX_VFS
		.file = NULL
#else
		.fd = -1
#endif
	};
	struct execve_arguments arguments = {0};
	struct posix_vm_context *vm_new = NULL;
	char *path_copy;
	char task_name[UK_POSIX_TASK_NAME_SIZE];
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_sigaction *signal_actions = NULL;
#endif
	struct ukarch_execenv *execenv_new;
	struct uk_thread *this_thread;
	struct ukarch_ctx ctx_old;
	struct uk_alloc *stack_allocator;
	void *stack_old;
	void *stack_new;
	__sz stack_size = STACK_SIZE;
	__sz stack_limit;
	int rc = 0;


	this_thread = uk_thread_current();
	UK_ASSERT(this_thread);

	path_copy = uk_malloc(uk_alloc_get_default(), PATH_MAX);
	if (!path_copy)
		return -ENOMEM;
	rc = uk_nofault_copy_string(path_copy, pathname, PATH_MAX);
	if (rc)
		goto err_free_path;
	pprocess_task_name_from_path(path_copy, task_name);
#if CONFIG_LIBPOSIX_VFS
	source.file = uk_posix_vfs_exec_open(path_copy, &source.path);
	if (PTRISERR(source.file)) {
		rc = PTR2ERR(source.file);
		source.file = NULL;
		goto err_free_path;
	}
#else
	source.fd = open(path_copy, O_RDONLY | O_CLOEXEC);
	if (source.fd < 0) {
		rc = -errno;
		goto err_free_path;
	}
#endif
	uk_mutex_lock(&pprocess_mutex);
	stack_limit = uk_pprocess_current()->limits[RLIMIT_STACK].rlim_cur;
	uk_mutex_unlock(&pprocess_mutex);
	rc = execve_arguments_init(&arguments, argv, envp, stack_limit,
				   strlen(path_copy) + 1);
	if (rc)
		goto err_free_path;

	stack_allocator = this_thread->_mem.stack_a;
	if (!stack_allocator)
		stack_allocator = uk_sched_current()->a_stack;

	loader_args.argv = arguments.argv;
	loader_args.envp = arguments.envp;
	loader_args.source = &source;
	loader_args.pathname = path_copy;
	loader_args.auxv = NULL;
	loader_args.image_data_size = 0;
	loader_args.stack_executable = false;
#if CONFIG_LIBPOSIX_VFS
	loader_args.executable = NULL;
#endif

	loader_args.progname = (char *)arguments.argv[0];

	/* Assign the default allocator to the loader. This will be used
	 * to allocate memory for the executable image.
	 */
	loader_args.alloc = uk_alloc_get_default();

#if CONFIG_LIBUKALLOCSTACK_PAGE_GUARDS
	stack_size = MIN(stack_limit,
			 (__sz)CONFIG_LIBPOSIX_PROCESS_STACK_SIZE);
	stack_size = UK_PAGING_PAGE_ALIGN_DOWN(stack_size);
	if (stack_size <= UKARCH_EXECENV_SIZE) {
		rc = -E2BIG;
		goto err_free_path;
	}
#endif

	/* Allocate a new stack. Even if we don't come from vfork we
	 * can't operate on the current thread's stack without either
	 * corrupting it or wasting space. Use the threads's
	 * stack allocator for the new stack.
	 */
	stack_new = uk_malloc(stack_allocator, stack_size);
	if (unlikely(!stack_new)) {
		uk_pr_err("Could not allocate stack\n");
		rc = -ENOMEM;
		goto err_free_path;
	}

	vm_new = pprocess_vm_alloc(uk_pprocess_current()->_a);
	if (!vm_new) {
		rc = -ENOMEM;
		goto err_free_stack_new;
	}

	vm_new->stack_size = stack_size;
	loader_args.stack_size = stack_size;
	loader_args.stack_bottom = (__uptr)stack_new;
	loader_args.stack_limit = stack_limit;
	loader_args.ctx.sp = ukarch_gen_sp((__uptr)stack_new,
					   loader_args.stack_size);

	uk_pr_debug("%s: New stack at %p - %p, stack pointer: %p\n",
		    loader_args.progname, stack_new,
		    (void *)((uintptr_t)stack_new + loader_args.stack_size),
		    (void *)loader_args.ctx.sp);

	/* Make room for the new execenv (context) to restore. Normally we
	 * would use the aux stack for this, and since this function is a
	 * noreturn we could even ovewrite the current auxsp frame. But
	 * just in case one day this is called outside the syscall context
	 * using the aux stack could corrupt the state of a syscall or the
	 * deferred exception handler. Avoid that by pushing instead into
	 * the newly allocated stack.
	 */
	loader_args.ctx.sp = ALIGN_DOWN(loader_args.ctx.sp,
					UKARCH_EXECENV_END_ALIGN);
	loader_args.ctx.sp -= UKARCH_EXECENV_SIZE;
	execenv_new = (struct ukarch_execenv *)loader_args.ctx.sp;

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	signal_actions = pprocess_signal_sigaction_alloc(uk_pprocess_current()->_a);
	if (!signal_actions) {
		rc = -ENOMEM;
		goto err_free_stack_new;
	}
#endif

	/* Load executable */
	uk_pthread_current()->exec_vm = vm_new;
	rc = uk_binfmt_load(&loader_args);
	uk_pthread_current()->exec_vm = NULL;
	execve_source_close(&source);
	if (unlikely(rc)) {
		uk_pr_err("%s: Unable to load (%d)\n", path_copy, rc);
		goto err_free_stack_new;
	}
	vm_new->image_data_size = loader_args.image_data_size;
#if CONFIG_LIBPOSIX_VFS
	vm_new->executable = loader_args.executable;
#endif
	rc = execve_auxv_init(vm_new, loader_args.auxv,
			      (const __uptr *)((char *)stack_new + stack_size));
	if (rc)
		goto err_free_stack_new;
#if CONFIG_LIBUKVMEM
	{
		__vaddr_t bottom = (__vaddr_t)stack_new;

		if (loader_args.stack_executable) {
			rc = pprocess_stack_executable(bottom);
			if (rc)
				goto err_free_stack_new;
		}
		rc = pprocess_stack_trim(vm_new, &bottom,
					 bottom + stack_size, loader_args.ctx.sp);
		if (rc)
			goto err_free_stack_new;
		stack_new = (void *)bottom;
	}
#endif

	/* Do arch-specific context (execenv) initialization */
	execve_arch_execenv_init(execenv_new, execenv,
				 loader_args.ctx.ip,
				 loader_args.ctx.sp);

	/* Prepare process for executing new context. First notify posix
	 * libraries that may have registered for the event, then do the
	 * internal cleanup.
	 */
	event_data = (struct posix_process_execve_event_data) {
		.thread = this_thread,
		.tid = ukthread2tid(this_thread),
		.pid = ukthread2pid(this_thread),
	};
	rc = pprocess_stop_others(uk_pprocess_current());
	if (rc)
		goto err_free_stack_new;
	rc = pprocess_raise_execve_event(&event_data);
	if (unlikely(rc < 0)) {
		uk_pr_err("execve event error (%d)\n", rc);
		goto err_free_stack_new;
	}
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	pprocess_signal_exec(signal_actions);
#endif

	/* Prepare switch to the new context.
	 *
	 * Update this thread's ctx and stack. We then need to free the old
	 * stack, and restore the new execenv (context). This needs to happen
	 * on the new stack, to avoid freeing the stack we are currently
	 * operating on, in case we are not on the auxstack. We do both via
	 * a context-switch trampoline that we execute on the new stack.
	 */
	stack_old = this_thread->_mem.stack;
	uk_pr_debug("%s: Old stack at %p - %p\n", loader_args.progname,
		    stack_old, (void *)((uintptr_t)stack_old + STACK_SIZE));

	rc = pprocess_task_name_set(this_thread, task_name);
	if (rc)
		goto err_free_stack_new;
	execve_arguments_free(&arguments);
	uk_free(uk_alloc_get_default(), path_copy);
	pprocess_vm_replace(this_thread, vm_new);
	this_thread->_mem.stack_a = stack_allocator;
	this_thread->_mem.stack = stack_new;
	this_thread->ctx = loader_args.ctx;

	ukarch_ctx_init_entry2(&loader_args.ctx,
			       loader_args.ctx.sp,
			       1, /* keep regs */
			       execve_ctx_switch,
			       (long)execenv_new, (long)stack_old);
	ukarch_ctx_switch(&ctx_old, &loader_args.ctx);
	UK_BUG(); /* noreturn */

err_free_stack_new:
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	if (signal_actions)
		pprocess_signal_sigaction_release(signal_actions);
#endif
	pprocess_vm_release_user(vm_new, uk_pprocess_current()->pid);
	uk_free(stack_allocator, stack_new);
err_free_path:
	execve_source_close(&source);
	execve_arguments_free(&arguments);
	uk_free(uk_alloc_get_default(), path_copy);
	return rc;
}
