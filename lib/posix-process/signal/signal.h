/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_PROCESS_SIGNAL_H__
#define __UK_PROCESS_SIGNAL_H__

#if !__ASSEMBLY__
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

#include <arch/signal.h>
#include <uk/alloc.h>
#include <uk/atomic.h>
#include <uk/list.h>
#include <uk/process.h>
#include <uk/posix-signal-wait.h>
#include <uk/refcount.h>
#include <uk/wait.h>

#include "process.h"
#include "sigset.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
#include "signal_file.h"
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */
#endif /* __ASSEMBLY__ */

#define UK_SIGNAL_RTMIN 32

#define SIG_ARRAY_COUNT		_NSIG /* SIGRTMAX + 1 */

#if !__ASSEMBLY__
/* Check if signal number is valid */
#define IS_VALID(_signum)						\
	((_signum) > 0 && (_signum) <= SIGRTMAX)

/* Check whether signal number is pending in sigqueue */
#define IS_PENDING(_sigqueue, _signum)					\
	!!(uk_load_n(&(_sigqueue).pending) & UK_BIT((_signum) - 1))

/* Set signal number in sigqueue */
#define SET_PENDING(_sigqueue, _signum)					\
	(void)uk_or(&(_sigqueue).pending, UK_BIT((_signum) - 1))

/* Clear a signal from a sigqueue */
#define RESET_PENDING(_sigqueue, _signum)				\
	(void)uk_and(&(_sigqueue).pending, ~UK_BIT((_signum) - 1))

/* Check if posix thread masks signal number */
#define IS_MASKED(_t, _signum)						\
	uk_sigismember(&(_t)->signal->mask, (_signum))

/* Sets a posix thread's mask of the given signal number */
#define SET_MASKED(_t, _signum)						\
	uk_sigaddset(&(_t)->signal->mask, (_signum))

/* Check if signal is ignored by a process. A signal is ignored if it
 * is explicitly set to IGNORE by sigaction, or if it is set to its
 * default action and that action is IGNORE.
 */
#define IS_IGNORED(_pproc, _signum)					\
	((KERN_SIGACTION(_pproc, _signum)->ks_handler == SIG_IGN) ||	\
	 (KERN_SIGACTION(_pproc, _signum)->ks_handler == SIG_DFL &&	\
	  UK_BIT(_signum) & SIGACT_IGN_MASK))

/* Helper to get pointer to kern_sigaction from process,
 * for a given signal
 */
#define KERN_SIGACTION(_proc, _signum)					\
	({								\
		UK_ASSERT((_proc)->signal);				\
		UK_ASSERT((_proc)->signal->sigaction);			\
		&(_proc)->signal->sigaction->ks[_signum];		\
	})

/* Helper to iterate over all signals (std + rt) */
#define pprocess_signal_foreach(_sn)					\
	for ((_sn) = 1; (__sz)(_sn) < SIG_ARRAY_COUNT; (_sn)++)

/* Helper to iterate over standard signals */
#define pprocess_stdsig_foreach(_sn)					\
	for ((_sn) = 1; (__sz)(_sn) < UK_SIGNAL_RTMIN; (_sn)++)

/* Kernel version of `struct sigaction` passed to rt_sigaction(). The order
 * of the elements of this struct is different of the `struct sigaction`
 * passed to the sigaction() glibc wrapper.
 */
struct kern_sigaction {
	void *ks_handler;
	unsigned long ks_flags;
	void (*ks_restorer)(void);
	uk_sigset_t ks_mask;
};

struct uk_sigaction {
	struct kern_sigaction ks[SIG_ARRAY_COUNT];
	struct uk_alloc *alloc;
	__atomic refcnt;
};

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

/* Descriptor of a pending signal.
 *
 * Each pending signal is described as a set of
 * information `siginfo`. This includes the signal
 * number, sending pid, address of faulting
 * instruction, and more. The individual fields of
 * `siginfo_t` are described in `man siginfo_t`.
 */
struct uk_signal;

enum uk_signal_dequeue_reason {
	UK_SIGNAL_CONSUME,
	UK_SIGNAL_DISCARD,
};

struct uk_signal_ops {
	bool coalesce_standard, bypass_limit, reserved_quota;
	void (*dequeue_locked)(struct uk_signal *signal,
			       enum uk_signal_dequeue_reason reason);
	void (*release)(struct uk_signal *signal);
};

struct uk_signal {
	siginfo_t siginfo;
	struct uk_list_head list_head;
	struct uk_alloc *allocator;
	const struct uk_signal_ops *ops;
	struct uk_signal *retired_next;
};

static inline struct uk_signal *uk_signal_alloc(struct uk_alloc *allocator)
{
	struct uk_signal *sig = uk_zalloc(allocator, sizeof(*sig));

	if (sig)
		sig->allocator = allocator;
	return sig;
}

static inline void uk_signal_free(struct uk_signal *sig)
{
	if (!sig)
		return;
	if (sig->ops) {
		UK_ASSERT(sig->ops->release);
		sig->ops->release(sig);
	} else {
		uk_free(sig->allocator, sig);
	}
}

/* Descriptor of a signal queue.
 *
 * Pending signals are expressed in a signal set, that
 * provides information on which signal numbers are pending.
 *
 * Ordinary standard signals coalesce. Owned notifications retain
 * separate entries until consumed or discarded.
 *
 * This structure maintains an list of `struct uk_signal` for
 * each signal number.
 */
struct uk_signal_queue {
	uk_sigset_t pending;
	struct uk_list_head list_head[SIG_ARRAY_COUNT];
};

struct posix_timer_context;

struct posix_timer {
	struct uk_list_head link;
	struct posix_timer_context *context;
	struct uk_posix_task *target;
	struct uk_signal signal;
	clockid_t clock, effective_clock;
	__u64 deadline, interval;
	int id, notify, overrun;
	bool real_interval, armed, deleted;
	bool loaned, queued, notification_current, notification_settled;
	bool waiting_signal;
};

struct posix_timer_context {
	struct posix_process *process;
	struct uk_signal_pdesc *signals;
	struct uk_list_head service_link, active, retired;
	struct posix_timer real;
	unsigned int next_id;
	bool attached, stopping;
};

/* Signal descriptor of a posix_process.
 *
 * A process may queue up to `queued_max` signals,
 * the value of which is set at init time.
 * POSIX requires at least _POSIX_SIGQUEUE_MAX
 * signals, while Linux allows the user to configure
 * that value via rlimit.
 *
 * Signals can be targeting the process, or individual
 * threads. Signals pending for the process are queued
 * in `sigqueue`. Signals pending for individual
 * threads are queued in thread-specific queues (see
 * `struct uk_signal_tdesc`).
 *
 * Each signal defines a signal action, which may
 * be the default one or a custom one specified by
 * calling `rt_sigaction`. Signal actions are defined
 * at the process level, i.e. are common for all
 * threads of a given process.
 */
struct uk_signal_pdesc {
	struct uk_alloc *allocator;
	__atomic references;
	__sz queued_count;
	__sz queued_max;
	__sz reserved_count, reserved_pending;
	struct posix_timer_context timers;
	struct uk_signal_queue sigqueue;
	/* We use dynamically allocated memory for sigaction as passing
	 * CLONE_SIGHAND to clone() requires that the parent and the child
	 * share the same set of handlers.
	 */
	struct uk_sigaction *sigaction;
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	struct uk_signal_files_ctx sigfiles_ctx;
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */
};

static inline bool
pprocess_signal_quota_full_locked(const struct uk_signal_pdesc *descriptor)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	return descriptor->reserved_count + descriptor->queued_count -
		       descriptor->reserved_pending >=
	       descriptor->queued_max;
}

/* Signal descriptor of a posix_thread.
 *
 * Each thread maintains its own signal mask.
 * That mask applies to signals targeting
 * that thread, and also impacts the delivery
 * of process-wide signals, as these are
 * delivered to the first thread that does
 * not mask that signal.
 *
 * Each thread also maintains its own signal
 * queue, as besides signals targeting the
 * process, signals can target individual
 * threads. For details on signal queues see
 * the description of `struct uk_signal_queue`.
 */
struct uk_signal_tdesc {
	stack_t altstack;
	uk_sigset_t mask;
	struct uk_signal_queue sigqueue;
	struct uk_waitq waitq;
	struct uk_posix_signal_wait *poll_wait;
	struct ukarch_execenv *syscall_context;
	__uptr syscall_pc;
	long syscall_number;
	long syscall_arg0;
	enum uk_posix_signal_restart syscall_restart;
	siginfo_t wait_info;
	uk_sigset_t saved_mask;
	bool notification;
	bool wait_pending;
	bool restore_mask;
};

void sys_error_handler(struct ukarch_execenv *execenv, long arg);

/* System error descriptor passed to handler */
struct sys_error_desc {
	int signum;
	int code;
	__uptr auxsp;
	__vaddr_t vaddr;
};

void pprocess_signal_deliver(struct ukarch_execenv *execenv);
void pprocess_signal_notify(struct posix_thread *thread);

/* Allocate signal descriptor of a posix process */
int pprocess_signal_pdesc_alloc(struct posix_process *proc);

/* Allocate signal descriptor of a posix thread */
int pprocess_signal_tdesc_alloc(struct posix_thread *thread);

/* Initialize signal descriptors of a posix process */
int pprocess_signal_pdesc_init(struct posix_process *proc);

/* Initialize signal descriptors of a posix thread */
void pprocess_signal_tdesc_init(struct posix_thread *thread);
void pprocess_signal_tdesc_reset_locked(struct posix_thread *thread,
					struct pprocess_retired *retired);
int pprocess_signal_inherit_locked(struct posix_thread *child,
				   struct posix_thread *parent, __u64 flags,
				   struct uk_sigaction **previous);

void pprocess_signal_pdesc_detach_locked(struct posix_process *process,
					 struct pprocess_retired *retired);
void pprocess_signal_tdesc_detach_locked(struct posix_thread *thread,
					 struct pprocess_retired *retired);
void pprocess_signal_pdesc_destroy(struct posix_process *process);
void pprocess_signal_pdesc_acquire(struct uk_signal_pdesc *descriptor);
void pprocess_signal_pdesc_put(struct uk_signal_pdesc *descriptor);
int pprocess_signal_enqueue(struct posix_process *process,
			    struct posix_thread *thread,
			    struct uk_signal *signal);
bool pprocess_signal_withdraw_locked(struct posix_process *process,
				     struct posix_thread *thread,
				     struct uk_signal *signal,
				     struct pprocess_retired *retired);
void pprocess_signal_tdesc_destroy(struct posix_thread *thread);

/* Dequeue a signal from a thread or process
 *
 * Setting pthread to NULL signifies that the signal
 * is queued at the process
 */
struct uk_signal *pprocess_signal_dequeue(struct posix_process *pproc,
					  struct posix_thread *pthread,
					  int signum);

void pprocess_signal_wake_waiter(struct posix_thread *thread);

struct uk_sigaction *pprocess_signal_sigaction_alloc(struct uk_alloc *alloc);

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
void pprocess_signal_exec(struct uk_sigaction *actions);
#endif

static inline void pprocess_signal_sigaction_acquire(struct uk_sigaction *sa)
{
	uk_refcount_acquire(&sa->refcnt);
}

static inline void pprocess_signal_sigaction_release(struct uk_sigaction *sa)
{
	if (uk_refcount_release(&sa->refcnt))
		uk_free(sa->alloc, sa);
}

/* Clears the fields of an instance of struct uk_sigaction */
void pprocess_signal_sigaction_clear(struct kern_sigaction *ks);

/* Clear pending signal(s) from process. If the signum corresponds to
 * an std-signal, it clears that an signal from pending. Otherwise,
 * if signum corresponding an rt-signal it clears all instances from
 * the queue.
 */
void pprocess_signal_clear_pending(struct posix_process *proc, int signum,
				   struct pprocess_retired *retired);

/* Caller holds pprocess_mutex. */
struct uk_posix_signal_wait;
int pprocess_signal_wait_check_locked(struct uk_posix_signal_wait *wait);

int pprocess_signal_take_locked(struct posix_thread *thread,
			        const uk_sigset_t *set, siginfo_t *info);

/* Caller holds pprocess_mutex once; the wait releases it while blocked. */
int pprocess_signal_wait_locked(struct posix_thread *thread,
			       const uk_sigset_t *set, siginfo_t *info,
			       __nsec deadline);

/* Signal a process (abstraction for kill / sigqueueinfo) */
int pprocess_signal_process_do(pid_t pid, int signum, siginfo_t *siginfo);

/* Signal a thread (abstraction for kill / sigqueueinfo) */
int pprocess_signal_thread_do(int tgid, int tid, int signum, siginfo_t *siginfo);

/* Signal a process */
int pprocess_signal_send(struct posix_process *proc, int signum,
			 siginfo_t *siginfo);

struct pprocess_signal_stack {
	stack_t previous;
	__uptr sp;
	bool on_altstack;
};

struct pprocess_signal_stack
pprocess_signal_stack_prepare(const struct kern_sigaction *action,
			      __uptr sp, __sz redzone);
int pprocess_signal_stack_reserve(const struct pprocess_signal_stack *stack,
				  __uptr bottom, __uptr top);
void pprocess_signal_bad_stack(void) __noreturn;
void pprocess_signal_arch_prepare(struct uk_signal *signal,
	const struct kern_sigaction *action, struct ukarch_execenv *execenv,
	const uk_sigset_t *saved_mask);
void pprocess_signal_restore_altstack(const stack_t *stack, __uptr sp);
void pprocess_signal_restore_mask(uk_sigset_t mask);

/* Checks whether a signal can be delivered to a given thread, depending
 * on the thread's mask, whether the process chooses to ingores this signal,
 * or whether the process uses a default disposition of ignore.
 *
 * Does NOT check permissions.
 */
bool pprocess_signal_is_deliverable(struct posix_thread *pthread, int signum);

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
/* Notify the signal files of a process */
static inline
void pprocess_signal_files_notify(struct posix_process *pproc, int signum)
{
	uk_signal_files_ctx_notify(&pproc->signal->sigfiles_ctx, signum);
}

/* Notify the signal files of a thread */
static inline
void pthread_signal_files_notify(struct posix_process *pproc,
				 struct posix_thread *pthread __unused,
				 int signum)
{
	/*
	 * TODO: Set file event for one specific thread only.
	 * We do it like this for now because we don't have individual
	 * thread notification yet
	 */
	uk_signal_files_ctx_notify(&pproc->signal->sigfiles_ctx, signum);
}
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNALFD */

/*
 * Check if a signal should be dropped: it is ignored by the process and
 * there are no signal files that are monitoring for this signal.
 */
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
static inline
bool pprocess_signal_should_drop(struct posix_process *pproc, int signum)
{
	return IS_IGNORED(pproc, signum) &&
	       !uk_signal_files_ctx_is_set(&pproc->signal->sigfiles_ctx,
					   signum);
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNALFD */
static inline
bool pprocess_signal_should_drop(struct posix_process *pproc, int signum)
{
	return IS_IGNORED(pproc, signum);
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNALFD */
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#endif /* !__ASSEMBLY__ */
#endif /* __UK_PROCESS_SIGNAL_H__ */
