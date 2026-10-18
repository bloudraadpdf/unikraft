/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Copyright (c) 2022, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
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

#ifndef __UK_PROCESS_H__
#define __UK_PROCESS_H__

#include <stdbool.h>
#include <errno.h>

#include <stddef.h> /* NULL */
#include <sys/resource.h>
#include <sys/types.h> /* pid_t */

#include <uk/config.h>
#include <uk/essentials.h>

#define UK_POSIX_TASK_NAME_SIZE 16

struct uk_posix_vm_view;

struct uk_thread;
int uk_posix_process_attach_application(struct uk_thread *thread,
					const char *path);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <arch/clone.h>
#include <uk/event.h>
#include <uk/prio.h>
#include <uk/thread.h>

#include <uk/process/events.h>
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING  */

int uk_sys_prlimit64(int pid, unsigned int resource,
		     const struct rlimit *new_limit, struct rlimit *old_limit);

static inline int uk_sys_getrlimit(int resource, struct rlimit *rlim)
{
	return rlim ? uk_sys_prlimit64(0, resource, NULL, rlim) : -EFAULT;
}

static inline int uk_sys_setrlimit(int resource, const struct rlimit *rlim)
{
	return rlim ? uk_sys_prlimit64(0, resource, rlim, NULL) : -EFAULT;
}

#if CONFIG_LIBPOSIX_FDTAB
unsigned int uk_posix_nofile_limit(void);
#endif

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBPOSIX_VFS
struct posix_process;
struct uk_posix_task;
struct uk_vfs_path;

enum uk_posix_task_state {
	UK_POSIX_TASK_RUNNABLE,
	UK_POSIX_TASK_BLOCKED,
	UK_POSIX_TASK_ZOMBIE
};

struct pu_credentials;

struct uk_posix_task_status {
	char name[UK_POSIX_TASK_NAME_SIZE];
	pid_t pid, tgid, ppid;
	size_t threads;
	enum uk_posix_task_state state;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *credentials;
	bool no_new_privs;
#endif
};

int uk_posix_process_task_status(struct posix_process *process,
				 struct uk_posix_task *task,
				 struct uk_posix_task_status *status);
void uk_posix_process_task_status_release(struct uk_posix_task_status *status);
#if CONFIG_LIBPOSIX_FDTAB
struct uk_fdtab;
struct uk_fdtab *uk_posix_process_task_fdtable(struct posix_process *process,
					       struct uk_posix_task *task);
#endif
struct posix_process *uk_posix_process_get(pid_t pid);
void uk_posix_process_put(struct posix_process *process);
bool uk_posix_process_attached(struct posix_process *process);
pid_t uk_posix_process_next_pid(pid_t after);
pid_t uk_posix_process_next_tid(struct posix_process *process, pid_t after);
struct uk_posix_task *uk_posix_process_task_get(struct posix_process *process,
						pid_t tid);
__u64 uk_posix_process_task_serial(const struct uk_posix_task *task);
void uk_posix_process_task_acquire(struct uk_posix_task *task);
void uk_posix_process_task_put(struct uk_posix_task *task);
bool uk_posix_process_task_attached(struct posix_process *process,
				    struct uk_posix_task *task);
int uk_posix_process_task_name(struct posix_process *process,
			       struct uk_posix_task *task,
			       char name[UK_POSIX_TASK_NAME_SIZE]);
int uk_posix_process_task_set_name(struct posix_process *process,
				   struct uk_posix_task *task,
				   const char name[UK_POSIX_TASK_NAME_SIZE]);
#if CONFIG_LIBUKVMEM
struct uk_posix_vm_view *
uk_posix_process_task_vm_view_get(struct posix_process *process,
				  struct uk_posix_task *task);
#endif
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
struct uk_vfs_path *
uk_posix_process_task_executable(struct posix_process *process,
				 struct uk_posix_task *task);
#endif
#endif

pid_t uk_sys_gettid(void);
pid_t uk_sys_getppid(void);
pid_t uk_sys_getpid(void);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

struct posix_vm_context;
void uk_posix_vm_reference_put(struct posix_vm_context *state);
bool uk_posix_process_exit_requested(void);
#if CONFIG_LIBPOSIX_FUTEX
void uk_posix_futex_lock(void);
void uk_posix_futex_unlock(void);
int uk_posix_futex_pi(__u32 *address, int command, bool private_key,
		      const __nsec *deadline,
		      const unsigned int *interrupted);
#endif
#if CONFIG_LIBUKVMEM
struct uk_vas;
struct uk_vma_ops;
struct uk_ofile;
struct uk_posix_vm_mapping {
	__vaddr_t start, end;
	unsigned long attr;
	bool private;
	const char *name;
	struct uk_ofile *file;
	__off offset;
};

void uk_posix_vm_view_put(struct uk_posix_vm_view *view);
int uk_posix_vm_maps(struct uk_posix_vm_view *view,
		     struct uk_posix_vm_mapping **output, size_t *count);
void uk_posix_vm_maps_release(struct uk_posix_vm_mapping *maps, size_t count);
int uk_posix_process_vm_map(struct uk_vas *vas, __vaddr_t *address, __sz length,
			    unsigned long attr, unsigned long flags,
			    const struct uk_vma_ops *ops, void *args);
int uk_posix_process_vm_unmap(struct uk_vas *vas, __vaddr_t address,
			      __sz length);
int uk_posix_process_vm_protect(struct uk_vas *vas, __vaddr_t address,
				__sz length, unsigned long attr,
				bool grow_down);
int uk_posix_process_vm_advise(struct uk_vas *vas, __vaddr_t address,
			       __sz length, unsigned long advice);
int uk_posix_process_vm_sync(struct uk_vas *vas, __vaddr_t address,
			     __sz length);
#endif

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#if CONFIG_LIBPOSIX_PROCESS_MULTIPROCESS

/**
 * Main-like function that calls exit() when done instead of returning.
 *
 * Parameters are passed as void *, and are to be cast by the implementation to:
 * - argc: int
 * - argv: char * []
 */
typedef __noreturn void (*uk_posix_process_mainlike_func)(void *argc,
							  void *argv);

/**
 * Spawn a process that jumps into function.
 *
 * DO NOT USE. This is only necessary when we create a new process
 * for main() in multiprocess.
 *
 * @param fn      Function to jump to.
 * @param argc    Arg count
 * @param argv    Arg vector
 * @return        Child pid to parent, or negative value on failure.
 */
pid_t uk_posix_process_run(uk_posix_process_mainlike_func fn, int argc,
			   const char **argv);
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTIPROCESS */

#endif /* __UK_PROCESS_H__ */
