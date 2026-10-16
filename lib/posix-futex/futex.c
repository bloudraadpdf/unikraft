/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Daniel Dinca <dincadaniel97@gmail.com>
 *          Marc Rittinghaus <marc.rittinghaus@kit.edu>
 *          Razvan Deaconescu <razvan.deaconescu@cs.pub.ro>
 *          Simon Kuenzer <simon.kuenzer@neclab.eu>
 *          Adina Smeu <adina.smeu@gmail.com>
 *
 * Copyright (c) 2021, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2021, Karlsruhe Institute of Technology (KIT).
 *                     All rights reserved.
 * Copyright (c) 2021, University Politehnica of Bucharest.
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

#include <uk/config.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <inttypes.h>

#include <linux/futex.h>
#include <uk/futex.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif
#include <uk/atomic.h>
#include <uk/thread.h>
#include <uk/thread.h>
#include <uk/list.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
#include <uk/sched.h>
#include <uk/list.h>
#include <uk/assert.h>
#include <uk/print.h>
#include <uk/spinlock.h>
#include <uk/lcpu.h>
#include <uk/plat/time.h>
#include <uk/posix-time.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif

/** @struct uk_futex
 *  @brief Futex structure.
 */
struct uk_futex {
	uint32_t *uaddr; /** The futex address. */
	struct uk_thread *thread; /** The thread waiting on the futex. */
	struct uk_list_head list_node; /** The list containing all the futexes
					 * on which the threads are waiting.
					 */
};

static UK_LIST_HEAD(futex_list);
static uk_spinlock futex_list_lock = UK_SPINLOCK_INITIALIZER();

bool uk_futex_has_waiters(__u32 *address)
{
	struct uk_futex *waiter;
	unsigned long flags;
	bool found = false;

	flags = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	uk_list_for_each_entry(waiter, &futex_list, list_node) {
		if (waiter->uaddr == address) {
			found = true;
			break;
		}
	}
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
	return found;
}


#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBPOSIX_PROCESS_SIGNAL
static __uk_tls bool pi_binary_call;
static __uk_tls __uptr pi_binary_address;

static void pi_restart_prepare(struct uk_syscall_enter_ctx *context)
{
	struct ukarch_execenv *environment = context->execenv;
	int command;

	if (!(context->flags & UK_SYSCALL_ENTER_CTX_BINARY_SYSCALL))
		return;
	pi_binary_call = false;
	if (_UK_EXECENV_REGS_GET(environment, __syscall_rsyscall) != SYS_futex)
		return;
	command = _UK_EXECENV_REGS_GET(environment, __syscall_rarg1) &
		FUTEX_CMD_MASK;
	if (command == FUTEX_LOCK_PI || command == FUTEX_LOCK_PI2) {
		pi_binary_address =
			_UK_EXECENV_REGS_GET(environment, __syscall_rarg0);
		pi_binary_call = true;
	}
}

static void pi_restart_complete(struct uk_syscall_exit_ctx *context)
{
	struct ukarch_execenv *environment = context->execenv;
	__uptr pc;

	if (!(context->flags & UK_SYSCALL_EXIT_CTX_BINARY_SYSCALL) ||
	    !pi_binary_call)
		return;
	pi_binary_call = false;
	if ((long)_UK_EXECENV_REGS_GET(environment, __syscall_rret0) != -EINTR)
		return;
	_UK_EXECENV_REGS_SET(environment, __syscall_rarg0, pi_binary_address);
	_UK_EXECENV_REGS_SET(environment, __syscall_rsyscall, SYS_futex);
	pc = uk_lcpu_regs_get(environment->regs, PC);
#if CONFIG_ARCH_X86_64
	_UK_EXECENV_REGS_SET(environment, __syscall_rret0, SYS_futex);
	pc -= 2;
#else
	pc -= 4;
#endif
	uk_lcpu_regs_set(environment->regs, PC, pc);
}

uk_syscall_entertab_prio(pi_restart_prepare, UK_PRIO_LATEST);
uk_syscall_exittab_prio(pi_restart_complete,
	UK_PRIO_BEFORE(UK_PRIO_BEFORE(UK_PRIO_LATEST)));
#endif

/**
 * Prepare to wait on a futex.
 *
 * Get the futex value atomically and compare it with the expected value. Add
 * the thread to the wait list and then block it if the value is equal to the
 * expected one. If the futex was not removed from the list when the thread was
 * unblocked, remove its entry before returning a timeout or interruption.
 *
 * @param uaddr		The futex userspace address
 * @param val		The expected value
 * @param timeout	The deadline until the function will block at most.
 * 			If it is NULL, the thread will wait indefinitely.
 *
 * @return
 *	0: uaddr contains val and the thread finished waiting;
 *	<1: -EAGAIN (uaddr does not contain val) or -ETIMEDOUT (the futex timed
 *       out)
 */
static int futex_wait_prepare(__u32 *uaddr, __u32 val,
			      const __nsec *timeout,
			      const unsigned int *interrupted)
{
	unsigned long irqf;
	struct uk_thread *current = uk_thread_current();
	struct uk_futex f = {.uaddr = uaddr, .thread = current};

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	int ret = uk_posix_futex_lock(uaddr, NULL);

	if (ret)
		return ret;
#endif
	irqf = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	if (uk_load_n(uaddr) != val) {
		uk_spin_unlock(&futex_list_lock);
		uk_lcpu_restore_irqf(irqf);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		uk_posix_futex_unlock();
#endif
		return -EAGAIN;
	}
	if (timeout && *timeout <= ukplat_monotonic_clock()) {
		uk_spin_unlock(&futex_list_lock);
		uk_lcpu_restore_irqf(irqf);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		uk_posix_futex_unlock();
#endif
		return -ETIMEDOUT;
	}
	uk_list_add_tail(&f.list_node, &futex_list);

	if (timeout) {
		/* Block at most until `timeout` nanosecs */
		uk_pr_debug("FUTEX_WAIT: Wait %"__PRIsnsec" nsec for wake-up\n",
				(__snsec) (*timeout));
		uk_thread_block_until(current, (__snsec) (*timeout));
	} else {
		/* Block indefinitely */
		uk_pr_debug("FUTEX_WAIT: Wait indefinitely for wake-up\n");
		uk_thread_block(current);
	}
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(irqf);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_posix_futex_unlock();
#endif
	/* A signal can arrive before uk_thread_block(). */
	if (interrupted && uk_load_n(interrupted))
		uk_thread_wake(current);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	if (uk_posix_process_exit_requested())
		uk_thread_wake(current);
#endif
	uk_sched_yield();

	uk_pr_debug("FUTEX_WAIT: Woke up (uaddr: %p)\n", uaddr);
	irqf = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);

	if (!uk_list_empty(&f.list_node)) {
		uk_list_del_init(&f.list_node);
		uk_spin_unlock(&futex_list_lock);
		uk_lcpu_restore_irqf(irqf);
		return timeout && *timeout <= ukplat_monotonic_clock() ?
			-ETIMEDOUT : -EINTR;
	}
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(irqf);

	return 0;
}

static int futex_wait(__u32 *uaddr, __u32 val, const __nsec *timeout)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_posix_signal_wait wait;
	int ret;

	ret = uk_posix_signal_wait_init(&wait, NULL, NULL, 0);
	if (ret)
		return ret;
	ret = uk_posix_signal_wait_check(&wait);
	if (!ret) {
		ret = futex_wait_prepare(uaddr, val, timeout,
					 &wait.interrupted);
		if (ret == -EINTR)
			ret = uk_posix_signal_wait_check(&wait);
	}
	uk_posix_signal_wait_end(&wait, ret);
	return ret;
#else
	return futex_wait_prepare(uaddr, val, timeout, NULL);
#endif
}

/**
 * Wake up threads waiting on a futex.
 *
 * Find val threads in the wait list for the futex, remove the futexes from the
 * list and wake up the threads.
 *
 * @param uaddr	The futex userspace address
 * @param val	The number of threads waiting on the futex to be woken up
 *
 * @return
 *	0: no threads were woken up;
 *	>0: the number of threads woken up
 */
static int futex_wake(uint32_t *uaddr, uint32_t val)
{
	unsigned long irqf;
	struct uk_list_head *itr, *tmp;
	struct uk_futex *f;
	uint32_t count = 0;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	int ret = uk_posix_futex_lock(uaddr, NULL);

	if (ret)
		return ret;
#endif
	irqf = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);

	uk_list_for_each_safe(itr, tmp, &futex_list) {
		f = uk_list_entry(itr, struct uk_futex, list_node);

		if (f->uaddr == uaddr) {
			/* Remove the thread from the futex list */
			uk_list_del_init(&f->list_node);

			/* TODO: Replace with uk_thread_wakeup when the new
			 * scheduler API is ready
			 */
			uk_thread_wake(f->thread);

			/* Wake at most val threads */
			if (++count >= val)
				break;
		}
	}

	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(irqf);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_posix_futex_unlock();
#endif
	return (int) count;
}

/**
 * Requeue waiters from uaddr to uaddr2.
 *
 * Requeue waiters on uaddr to uaddr2. Wakes up a maximum of val waiters that
 * are waiting on the futex at uaddr.  If there are more than val waiters, then
 * the remaining waiters are removed from the wait queue of the source futex at
 * uaddr and added to the wait queue of the target futex at uaddr2. The val2
 * argument specifies an upper limit on the number of waiters that are requeued
 * to the futex at uaddr2.
 *
 * @param uaddr		Source futex user address
 * @param val		Number of waiters to wake
 * @param val2		Number of waiters to requeue (0-INT_MAX)
 * @param uaddr2	Target futex user address
 * @param expected	Optional uaddr expected value
 *
 * @return
 *	>=0: on success, the number of tasks requeued or woken;
 *	<0: on error
 */
static int futex_requeue(__u32 *uaddr, __u32 val, __u32 val2,
			 __u32 *uaddr2, const __u32 *expected)
{
	unsigned long irqf;
	struct uk_list_head *itr, *tmp;
	struct uk_futex *f;
	__u32 woken = 0;
	__u32 requeued = 0;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	int ret = uk_posix_futex_lock(uaddr, uaddr2);

	if (ret)
		return ret;
#endif
	irqf = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	if (expected && *expected != uk_load_n(uaddr)) {
		uk_spin_unlock(&futex_list_lock);
		uk_lcpu_restore_irqf(irqf);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		uk_posix_futex_unlock();
#endif
		return -EAGAIN;
	}

	uk_list_for_each_safe(itr, tmp, &futex_list) {
		f = uk_list_entry(itr, struct uk_futex, list_node);
		if (f->uaddr != uaddr)
			continue;
		if (woken < val) {
			uk_list_del_init(&f->list_node);
			uk_thread_wake(f->thread);
			woken++;
		} else if (requeued < val2) {
			f->uaddr = uaddr2;
			requeued++;
		} else {
			break;
		}
	}

	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(irqf);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_posix_futex_unlock();
#endif
	return woken + requeued;
}

static int futex_deadline(const struct timespec *timeout, int cmd,
			  int flags, __nsec *deadline)
{
	struct timespec clock;
	__nsec target;
	__nsec now = ukplat_monotonic_clock();
	__nsec duration;
	int ret;

	if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
	    timeout->tv_nsec >= UKARCH_NSEC_PER_SEC)
		return -EINVAL;
	if ((__u64)timeout->tv_sec >
	    (INT64_MAX - timeout->tv_nsec) / UKARCH_NSEC_PER_SEC)
		target = INT64_MAX;
	else
		target = ukarch_time_sec_to_nsec(timeout->tv_sec) +
			timeout->tv_nsec;
	if (cmd == FUTEX_WAIT_BITSET || cmd == FUTEX_LOCK_PI ||
	    cmd == FUTEX_LOCK_PI2) {
		if (cmd != FUTEX_LOCK_PI && !(flags & FUTEX_CLOCK_REALTIME)) {
			*deadline = target;
			return 0;
		}
		ret = uk_sys_clock_gettime(CLOCK_REALTIME, &clock);
		if (ret)
			return ret;
		duration = ukarch_time_sec_to_nsec(clock.tv_sec) +
			clock.tv_nsec;
		target = target > duration ? target - duration : 0;
	}
	*deadline = target > INT64_MAX - now ? INT64_MAX : now + target;
	return 0;
}

/**
 * According to man pages, there exists no libc wrapper for futex
 *
 * @param uaddr		Source futex user address
 * @param futex_op	Futex operation
 * @param val		Depends on the futex operation
 * @param timeout	Usually a timeout for the futex operation, but can be
 *			used as an integer value for other operations
 * @param uaddr2	Optional target futex user address
 * @param val3		Depends on the futex operation
 *
 * @return
 *	>=0: on success
 *	<0: on error
 */
UK_LLSYSCALL_R_DEFINE(int, futex, uint32_t *, uaddr, int, futex_op,
		      uint32_t, val, const struct timespec *, timeout,
		      uint32_t *, uaddr2, uint32_t, val3)
{
	__nsec timeout_ns;
	int cmd = futex_op & FUTEX_CMD_MASK;
	int ret;

	/* Reject invalid combinations of the realtime clock flag */
	if (futex_op & FUTEX_CLOCK_REALTIME && !(
		cmd == FUTEX_WAIT ||
		cmd == FUTEX_WAIT_BITSET ||
		cmd == FUTEX_LOCK_PI2 ||
		cmd == FUTEX_WAIT_REQUEUE_PI))
		return -ENOSYS;

	if (timeout && (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET ||
			cmd == FUTEX_LOCK_PI || cmd == FUTEX_LOCK_PI2)) {
		ret = futex_deadline(timeout, cmd, futex_op, &timeout_ns);
		if (ret)
			return ret;
	}
	switch (cmd) {
	case FUTEX_WAIT:
		return futex_wait(uaddr, val, timeout ? &timeout_ns : NULL);

	case FUTEX_WAIT_BITSET:
		/*
		 * Special case implementation for cases where the val3/mask has
		 * all bits set. Some applications do this to use the absolute
		 * timeout mode. We return ENOSYS for all non-supported calls.
		 */
		if (val3 != UINT32_MAX)
			return -ENOSYS;

		return futex_wait(uaddr, val, timeout ? &timeout_ns : NULL);

	case FUTEX_WAKE:
		return futex_wake(uaddr, val);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	case FUTEX_TRYLOCK_PI:
	case FUTEX_UNLOCK_PI:
		return uk_posix_futex_pi(uaddr, cmd, NULL, NULL);
	case FUTEX_LOCK_PI:
	case FUTEX_LOCK_PI2:
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	{
		struct uk_posix_signal_wait wait;

		ret = uk_posix_signal_wait_init(&wait, NULL, NULL, 0);
		if (ret)
			return ret;
		ret = uk_posix_signal_wait_check(&wait);
		if (!ret)
			ret = uk_posix_futex_pi(uaddr, cmd,
				timeout ? &timeout_ns : NULL,
				&wait.interrupted);
		if (ret == -EINTR)
			ret = uk_posix_signal_wait_check(&wait);
		uk_posix_signal_wait_end(&wait, ret);
		return ret;
	}
#else
		return uk_posix_futex_pi(uaddr, cmd,
			timeout ? &timeout_ns : NULL, NULL);
#endif
#endif
	case FUTEX_FD:
		return -ENOSYS;

	case FUTEX_REQUEUE:
		return futex_requeue(uaddr, val, (unsigned long)timeout,
				     uaddr2, NULL);

	case FUTEX_CMP_REQUEUE:
		return futex_requeue(uaddr, val, (unsigned long)timeout,
				     uaddr2, &val3);

	default:
		return -ENOSYS;
		/* TODO: other operations? */
	}
}

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
/*
 * Reference to child TID that should be cleared on thread exit
 * (if not NULL):
 *  https://man7.org/linux/man-pages/man2/set_tid_address.2.html
 */
static __uk_tls pid_t *child_tid_clear_ref = NULL;

/* Store reference for clearing child TID on exit */
static int pfutex_child_cleartid(void *arg)
{
	struct posix_process_clone_event_data *event_data;
	const struct clone_args *cl_args;
	struct uk_thread *child;
	pid_t *child_tid;

	event_data = (struct posix_process_clone_event_data *)arg;
	UK_ASSERT(event_data);

	cl_args = event_data->cl_args;
	UK_ASSERT(cl_args);

	if (!(cl_args->flags & CLONE_CHILD_CLEARTID))
		return UK_EVENT_NOT_HANDLED;

	child_tid = (pid_t *)cl_args->child_tid;
	child = event_data->child;

	uk_thread_uktls_var(child, child_tid_clear_ref) = child_tid;

	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_CLONE_HANDLER(CLONE_CHILD_CLEARTID, pfutex_child_cleartid);

/* Update the stored reference for clearing child TID on exit
 * NOTE: There is no libc wrapper
 * NOTE: The system call also returns the caller's thread ID.
 *       We call SYS_gettid effectively and in case of an error
 *       we forward the error code to the caller. In such a case,
 *       we also do not update the reference.
 */
UK_LLSYSCALL_R_DEFINE(pid_t, set_tid_address, pid_t *, tid_ref)
{
	pid_t self_tid = uk_sys_gettid();

	if (self_tid >= 0) {
		/* Store new reference */
		child_tid_clear_ref = tid_ref;
	}
	return self_tid;
}

static int pthread_exit_handler(void *arg)
{
	struct posix_process_ptexit_event_data *event_data;
	struct uk_list_head *itr, *tmp;
	struct uk_thread *child;
	struct uk_futex *f;
	pid_t *clear_ref;

	UK_ASSERT(arg);

	event_data = (struct posix_process_ptexit_event_data *)arg;
	child = event_data->thread;

	/* Clear child TID at the stored reference */
	clear_ref = uk_thread_uktls_var(child, child_tid_clear_ref);
	if (clear_ref) {
		uk_store_n(clear_ref, 0);
		futex_wake((uint32_t *)clear_ref, 1);
	}

	/* Clear this thread's entries from the list */
	uk_spin_lock(&futex_list_lock);
	uk_list_for_each_safe(itr, tmp, &futex_list) {
		f = uk_list_entry(itr, struct uk_futex, list_node);
		if (f->thread == child) {
			uk_list_del_init(&f->list_node);
			break; /* a thread can wait on one futex */
		}
	}
	uk_spin_unlock(&futex_list_lock);

	return 0;
}

POSIX_PROCESS_PTEXIT_HANDLER_PRIO(pthread_exit_handler,
				  UK_PRIO_AFTER(UK_PRIO_EARLIEST));

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
