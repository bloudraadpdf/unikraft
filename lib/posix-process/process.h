/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2022, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */
#ifndef __PROCESS_H_INTERNAL__
#define __PROCESS_H_INTERNAL__

#include <sys/types.h>
#include <uk/config.h>
#include <uk/mutex.h>
#include "task.h"

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <linux/sched.h>
#include <uk/arch/ctx.h>
#include <uk/process.h>
#include <uk/semaphore.h>
#include <uk/thread.h>
#include <uk/mutex.h>
#include <uk/refcount.h>
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

extern struct uk_thread *pprocess_thread_main;
extern struct uk_mutex pprocess_mutex;

extern int pprocess_exit_status;

/* Defined as a value larger than the max error code
 * to distinguish between set to zero and unset.
 */
#define PPROCESS_EXIT_STATUS_UNSET  (1 << 8)

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

#define UK_PID_INIT		1
#define TIDMAP_SIZE		(CONFIG_LIBPOSIX_PROCESS_MAX_PID + 1)

struct uk_signal;

struct pprocess_retired {
	struct posix_vm_context *vms;
	struct posix_thread *threads;
	struct posix_process *processes;
	struct uk_posix_task *tasks;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_signal *signals;
#endif
};

void pprocess_retired_release(struct pprocess_retired *retired);
void pprocess_retire_thread_locked(struct posix_thread *thread,
				   struct pprocess_retired *retired);
void pprocess_retire_process_locked(struct posix_process *process,
				    struct pprocess_retired *retired);

/* Notice: The RUNNING state is not necessarily in sync with the state
 * of the underlying uk_thread (may be blocked by the scheduler).
 * On the other hand, the BLOCKED state implies that the underlying
 * uk_thread is also blocked. Use RUNNING only as a means to check
 * whether a posix-thread is neither terminated or blocked.
 */
enum posix_thread_state {
	POSIX_THREAD_RUNNING,
	POSIX_THREAD_BLOCKED_VFORK,  /* waiting for child to call execve */
	POSIX_THREAD_BLOCKED_WAIT,   /* waiting for process state change */
	POSIX_THREAD_BLOCKED_SIGNAL, /* waiting for signal */
	POSIX_THREAD_EXITED,         /* terminated normally */
	POSIX_THREAD_KILLED,         /* terminated by signal */
};

enum posix_process_state {
	POSIX_PROCESS_RUNNING,        /* not terminated */
	POSIX_PROCESS_EXITED,         /* terminated normally */
	POSIX_PROCESS_KILLED,         /* terminated by signal */
};

struct posix_vm_context;

struct uk_posix_vm_view {
	struct posix_vm_context *context;
};

struct posix_vm_context {
	struct posix_vm_context *retired_next;
	struct uk_alloc *allocator;
	unsigned int users;
	unsigned int references;
	struct uk_posix_vm_view view;
	unsigned int membarrier_registered;
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	void *auxv;
	__sz auxv_size;
	__sz stack_size;
	__vaddr_t stack_base;
	__vaddr_t stack_top;
	__sz image_data_size;
#if CONFIG_LIBPOSIX_VFS
	struct uk_vfs_path *executable;
#endif
#endif
#if CONFIG_LIBPOSIX_PROCESS_BRK
	struct {
		void *base;
		__sz pos;
	} brk;
#endif
};

struct posix_process {
	struct posix_process *retired_next;
	__atomic references;
	pid_t pid;
	struct posix_process *parent;
	struct uk_list_head children; /* child processes */
	struct uk_list_head child_list_entry;
	struct uk_list_head threads;
	struct uk_alloc *_a;
	struct posix_vm_context *vm_context;
	struct rlimit limits[RLIM_NLIMITS];
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_signal_pdesc *signal;
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */
	struct uk_waitq child_wait;
	struct uk_waitq thread_wait;
	struct uk_thread *exec_owner;
	enum posix_process_state exit_state;
	struct uk_semaphore exit_semaphore;
	enum posix_process_state state;
	__u64 exit_signal;
	int exit_status;


	/* TODO: Mutex */
};

struct posix_thread {
	struct posix_thread *retired_next;
	struct posix_task task;
	pid_t tid;
	void *rseq_area;
	__u32 rseq_length;
	__u32 rseq_signature;
	void *robust_list;
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
	struct sysv_sem_undo *sem_undo;
#endif
#if CONFIG_LIBPOSIX_PROCESS_PREEMPT
	__u64 application_deadline;
#endif
	struct posix_process *process;
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	struct posix_vm_context *exec_vm;
#endif
	struct posix_thread *vfork_parent;
	struct uk_list_head thread_list_entry;
	struct uk_thread *thread;
	struct uk_alloc *_a;
	enum posix_thread_state state;
	enum posix_thread_state exit_state;
	int exit_status;
	bool user_started;
	bool cleanup_started;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_signal_tdesc *signal;
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

	/* TODO: Mutex */
};

extern struct posix_process *pid_process[TIDMAP_SIZE];
struct posix_vm_context *pprocess_vm_alloc(struct uk_alloc *allocator);

int pprocess_vm_init(struct posix_process *process, struct posix_process *parent);
void pprocess_vm_release_user(struct posix_vm_context *state, pid_t pid);
void pprocess_vm_retire_locked(struct posix_vm_context *state, pid_t pid,
			       struct pprocess_retired *retired);
void pprocess_vm_destroy_retired(struct posix_vm_context *state);
#if CONFIG_LIBUKVMEM
struct uk_vma_ops;
void pprocess_vm_unmap_owned(struct posix_vm_context *state,
			     const struct uk_vma_ops *ops);
#endif
void pprocess_thread_cleanup(struct uk_thread *thread);
void pprocess_vm_replace(struct uk_thread *thread,
			 struct posix_vm_context *state);
#if CONFIG_LIBPOSIX_PROCESS_BRK
int pprocess_brk_init(struct posix_vm_context *state);
#if CONFIG_LIBUKVMEM
void pprocess_brk_commit(struct posix_vm_context *state);
#else
void pprocess_brk_free(struct posix_vm_context *state);
#endif
#endif
void pprocess_thread_fini(struct uk_thread *thread);
void pprocess_resource_init(struct posix_process *process,
			    struct posix_process *parent);
#if CONFIG_LIBUKVMEM && CONFIG_LIBPOSIX_PROCESS_BRK
int pprocess_memory_brk(struct posix_vm_context *state, __sz position);
#endif
#if CONFIG_LIBUKVMEM && CONFIG_LIBPOSIX_PROCESS_EXECVE
int pprocess_memory_stack(__vaddr_t address);
int pprocess_stack_executable(__vaddr_t address);
int pprocess_stack_trim(struct posix_vm_context *state,
			__vaddr_t *bottom, __vaddr_t top, __vaddr_t sp);
#endif
#if CONFIG_LIBPOSIX_PROCESS_BRK
#define PPROCESS_BRK_PAGES UK_BIT(CONFIG_LIBPOSIX_PROCESS_BRK_PAGE_ORDER)
#define PPROCESS_BRK_LEN (UK_PAGING_PAGE_SIZE * PPROCESS_BRK_PAGES)
#endif
void pprocess_finish_exit(struct posix_process *process,
			  struct posix_thread *thread,
			  struct pprocess_retired *retired);
int pprocess_stop_others(struct posix_process *process);
void pprocess_enter_application(void);
void __noreturn pprocess_execenv_load(long execenv);

static inline bool pprocess_thread_exit_requested(struct posix_thread *thread)
{
	return uk_load_n(&thread->exit_state) != POSIX_THREAD_RUNNING;
}

extern __uk_tls struct posix_thread *pthread_self;

#define uk_pprocess_foreach(_p)						\
	for (int _j = 1, _i = 0; _i != ARRAY_SIZE(pid_process);		\
		_j = !_j, _i++)						\
			for ((_p) = pid_process[_i]; _j; _j = !_j)	\
				if ((_p))

#define uk_pprocess_foreach_pthread(_proc, _pthread, _pthreadn)		\
	uk_list_for_each_entry_safe((_pthread), (_pthreadn),		\
				    &(_proc)->threads, thread_list_entry)

#define uk_pprocess_foreach_child(_proc, _pchild, _pchildn)		\
	uk_list_for_each_entry_safe((_pchild), (_pchildn), &(_proc)->children, \
				    child_list_entry)

#define uk_pthread_current()						\
	uk_thread_uktls_var(uk_thread_current(), pthread_self)

#define uk_pprocess_current() uk_pthread_current()->process

void pprocess_pi_update(struct uk_thread *changed);
#if CONFIG_LIBPOSIX_FUTEX
int pprocess_pi_owner_died(struct uk_thread *thread, __u32 *address, pid_t tid);
#endif

struct uk_posix_task *pprocess_task_get_locked(struct posix_process *process,
					       pid_t tid);
struct posix_thread *
pprocess_task_resolve_locked(struct posix_process *process,
			     struct uk_posix_task *identity);
void pprocess_task_put(struct uk_posix_task *identity);

struct posix_process *pid2pprocess(pid_t pid);
struct uk_thread *tid2ukthread(pid_t tid);
struct posix_thread *tid2pthread(pid_t tid);
struct posix_process *tid2pprocess(pid_t tid);
pid_t ukthread2tid(struct uk_thread *thread);
pid_t ukthread2pid(struct uk_thread *thread);

void pprocess_exit_pthread(struct posix_thread *thread,
			   enum posix_thread_state state,
			   struct pprocess_retired *retired);
void pprocess_exit(struct posix_process *process,
		   enum posix_process_state state,
		   int exit_status);

/**
 * INTERNAL. Release pthread
 *
 * Releases thread resources.
 *
 * @param pthread The thread to release.
 */
void pprocess_release_pthread(struct posix_thread *pthread,
			      struct pprocess_retired *retired);
void pprocess_complete_vfork(struct posix_thread *pthread);

/**
 * INTERNAL. Release process
 *
 * Releases process resources and removes from process table.
 * Requires that all pthreads are already terminated.
 *
 * @param pprocess  The process to release.
 */
void pprocess_release(struct posix_process *pprocess,
		      struct pprocess_retired *retired);
void pprocess_acquire(struct posix_process *process);
void pprocess_put(struct posix_process *process);

/**
 * INTERNAL. Create pthread
 *
 * @param pprocess process to assign thread to
 * @param thread   backing uk_thread to create pthread from
 * @return pthread on success or negative value on failure
 */
struct posix_thread *pprocess_create_pthread(struct posix_process *pprocess,
					     struct uk_thread *thread,
					     struct uk_thread *creator);
/**
 * INTERNAL. Create process
 *
 * @param alloc  allocator to assign process
 * @param thread backing uk_thread to create process from
 * @param parent parent processs
 * @return process on success or negative value on failure
 */
struct posix_process *pprocess_create(struct uk_alloc *a,
				      struct uk_thread *thread,
				      struct uk_thread *parent);

int uk_clone(struct clone_args *cl_args, size_t cl_args_len,
	     struct ukarch_execenv *execenv);

/**
 * INTERNAL. POSIX process events kernel API
 *
 * Handler parameters are defined in the public API
 * (libposix-process/include/uk/process/event.h)
 */

/**
 * INTERNAL. Raises clone event
 *
 * @param data Pointer to event data.
 * @return UK_EVENT_HANDLED_CONT, UK_EVENT_NOT_HANDLED,
 *         or negative value on error.
 */
int pprocess_raise_clone_event(struct posix_process_clone_event_data *data);

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
/**
 * INTERNAL. Raises exceve event
 *
 * @param data Pointer to event data.
 * @return UK_EVENT_HANDLED_CONT, UK_EVENT_NOT_HANDLED,
 *         or negative value on error.
 */
int pprocess_raise_execve_event(struct posix_process_execve_event_data *data);
#endif /* CONFIG_LIBPOSIX_PROCESS_EXECVE */

/**
 * INTERNAL. Raises pthread exit event
 *
 * @param data Pointer to event data.
 */
void pprocess_raise_ptexit_event(struct posix_process_ptexit_event_data *data);

/**
 * INTERNAL. Raises process exit event
 *
 * @param data Pointer to event data.
 */
void pprocess_raise_ppexit_event(struct posix_process_ppexit_event_data *data);

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
#endif /* __PROCESS_H_INTERNAL__ */
