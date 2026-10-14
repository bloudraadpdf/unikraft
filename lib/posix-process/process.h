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

extern int pprocess_exit_status;

/* Defined as a value larger than the max error code
 * to distinguish between set to zero and unset.
 */
#define PPROCESS_EXIT_STATUS_UNSET  (1 << 8)

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

#define UK_PID_INIT		1
#define TIDMAP_SIZE		(CONFIG_LIBPOSIX_PROCESS_MAX_PID + 1)

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

struct posix_vm_context {
	struct uk_alloc *allocator;
	unsigned int references;
	unsigned int membarrier_registered;
#if CONFIG_LIBPOSIX_PROCESS_BRK
	struct {
		struct uk_mutex mtx;
		void *base;
		__sz pos;
	} brk;
#endif
};

struct posix_process {
	__atomic references;
	pid_t pid;
	struct posix_process *parent;
	struct uk_list_head children; /* child processes */
	struct uk_list_head child_list_entry;
	struct uk_list_head threads;
	struct uk_alloc *_a;
	struct posix_vm_context *vm_context;
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
	pid_t tid;
	struct posix_process *process;
	struct posix_thread *vfork_parent;
	struct uk_list_head thread_list_entry;
	struct uk_thread *thread;
	struct uk_alloc *_a;
	enum posix_thread_state state;
	enum posix_thread_state exit_state;
	int exit_status;
	bool user_started;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_signal_tdesc *signal;
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

	/* TODO: Mutex */
};

extern struct posix_process *pid_process[TIDMAP_SIZE];
struct posix_vm_context *pprocess_vm_alloc(struct uk_alloc *allocator);
int pprocess_vm_init(struct posix_process *process, struct posix_process *parent);
void pprocess_vm_put(struct posix_vm_context *state);
void pprocess_vm_replace(struct posix_process *process,
			struct posix_vm_context *state);
#if CONFIG_LIBPOSIX_PROCESS_BRK
int pprocess_brk_init(struct posix_vm_context *state);
void pprocess_brk_free(struct posix_vm_context *state);
#endif
extern struct uk_mutex pprocess_mutex;
void pprocess_finish_exit(struct posix_process *process,
                         struct posix_thread *thread);
int pprocess_stop_others(struct posix_process *process);
void pprocess_enter_application(void);
#if CONFIG_ARCH_ARM_64
void __noreturn pprocess_execenv_load(long execenv);
#endif

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
	uk_list_for_each_entry_safe((_pchild), (_pchildn),		\
				    &(_proc)->children, child_list_entry)

#define uk_pthread_current()						\
	uk_thread_uktls_var(uk_thread_current(), pthread_self)

#define uk_pprocess_current()						\
	uk_pthread_current()->process

struct posix_process *pid2pprocess(pid_t pid);
struct uk_thread *tid2ukthread(pid_t tid);
struct posix_thread *tid2pthread(pid_t tid);
struct posix_process *tid2pprocess(pid_t tid);
pid_t ukthread2tid(struct uk_thread *thread);
pid_t ukthread2pid(struct uk_thread *thread);

void pprocess_exit_pthread(struct posix_thread *thread,
                          enum posix_thread_state state);
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
void pprocess_release_pthread(struct posix_thread *pthread);
void pprocess_complete_vfork(struct posix_thread *pthread);

/**
 * INTERNAL. Release process
 *
 * Releases process resources and removes from process table.
 * Requires that all pthreads are already terminated.
 *
 * @param pprocess  The process to release.
 */
void pprocess_release(struct posix_process *pprocess);
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
					     struct uk_thread *thread);
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
