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
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <linux/futex.h>
#include <uk/alloc.h>
#include <uk/atomic.h>
#include <uk/futex.h>
#include <uk/lcpu.h>
#include <uk/nofault.h>
#include <uk/plat/time.h>
#include <uk/posix-time.h>
#include <uk/sched.h>
#include <uk/spinlock.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif

static UK_LIST_HEAD(futex_list);
static uk_spinlock futex_list_lock = UK_SPINLOCK_INITIALIZER();

bool uk_futex_pi_conflict(const struct uk_futex_key *key)
{
	struct uk_futex_waiter *waiter;
	unsigned long flags;
	bool found = false;

	flags = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	uk_list_for_each_entry(waiter, &futex_list, link) {
		if (uk_futex_key_equal(waiter->key, key)) {
			found = !waiter->is_pi;
			break;
		}
	}
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
	return found;
}

static void futex_queue_insert(struct uk_futex_waiter *waiter)
{
	struct uk_futex_waiter *cursor;

	uk_list_for_each_entry(cursor, &futex_list, link) {
		if (waiter->queue_priority > cursor->queue_priority) {
			uk_list_add_tail(&waiter->link, &cursor->link);
			return;
		}
	}
	uk_list_add_tail(&waiter->link, &futex_list);
}

void uk_futex_queue_add(struct uk_futex_waiter *waiter)
{
	unsigned long flags = uk_lcpu_save_irqf();

	uk_spin_lock(&futex_list_lock);
	waiter->queue_priority = uk_load_n(&waiter->thread->sched_priority);
	futex_queue_insert(waiter);
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
}

void uk_futex_queue_remove(struct uk_futex_waiter *waiter)
{
	unsigned long flags = uk_lcpu_save_irqf();

	uk_spin_lock(&futex_list_lock);
	uk_list_del_init(&waiter->link);
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
}

static void futex_lock(unsigned long *flags)
{
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_posix_futex_lock();
#endif
	*flags = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
}

static void futex_unlock(unsigned long flags)
{
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_posix_futex_unlock();
#endif
}

static int futex_wait_prepare(__u32 *address, __u32 value, bool private_key,
			      const __nsec *deadline,
			      const unsigned int *interrupted)
{
	struct uk_thread *thread = uk_thread_current();
	struct uk_futex_call *call =
		uk_futex_call_create(thread, uk_alloc_get_default());
	struct uk_futex_call *retired = NULL;
	struct uk_futex_waiter *waiter;
	unsigned long flags;
	__u32 observed;
	int rc;

	if (!call)
		return -ENOMEM;
	waiter = &call->waiter;
	waiter->address = address;
	waiter->result = -EINTR;
	for (;;) {
		uk_futex_key_put(&call->keys[0]);
		rc = uk_futex_key_get(thread, address, private_key, false,
				      &call->keys[0]);
		if (rc)
			goto out;
		rc = uk_futex_key_fault(address, false, &call->keys[0]);
		if (rc)
			goto out;
		futex_lock(&flags);
		rc = uk_nofault_read32(address, &observed,
				       UK_NOFAULTF_NOPAGING);
		if (rc) {
			futex_unlock(flags);
			continue;
		}
		if (observed != value) {
			rc = -EAGAIN;
			goto unlock;
		}
		if (deadline && *deadline <= ukplat_monotonic_clock()) {
			rc = -ETIMEDOUT;
			goto unlock;
		}
		uk_futex_waiter_key_set(waiter, call, 0, &retired);
		waiter->queue_priority = uk_load_n(&thread->sched_priority);
		futex_queue_insert(waiter);
		if (deadline)
			uk_thread_block_until(thread, *deadline);
		else
			uk_thread_block(thread);
		futex_unlock(flags);
		uk_futex_retired_release(retired);
		break;
	}
	if (interrupted && uk_load_n(interrupted))
		uk_thread_wake(thread);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	if (uk_posix_process_exit_requested())
		uk_thread_wake(thread);
#endif
	uk_sched_yield();
	flags = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	rc = waiter->result;
	if (!uk_list_empty(&waiter->link)) {
		uk_list_del_init(&waiter->link);
		rc = deadline && *deadline <= ukplat_monotonic_clock() ?
			-ETIMEDOUT : -EINTR;
	}
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
	goto out;
unlock:
	futex_unlock(flags);
out:
	uk_futex_call_end(thread, call);
	return rc;
}

static int futex_wait(__u32 *address, __u32 value, bool private_key,
		      const __nsec *deadline)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_posix_signal_wait wait;
	int rc;

	rc = uk_posix_signal_wait_init(&wait, NULL, NULL, 0);
	if (rc)
		return rc;
	rc = uk_posix_signal_wait_check(&wait);
	if (!rc) {
		rc = futex_wait_prepare(address, value, private_key, deadline,
					&wait.interrupted);
		if (rc == -EINTR)
			rc = uk_posix_signal_wait_check(&wait);
	}
	uk_posix_signal_wait_end(&wait, rc, deadline ?
		UK_POSIX_SIGNAL_RESTART_NONE : UK_POSIX_SIGNAL_RESTART_HANDLER);
	return rc;
#else
	return futex_wait_prepare(address, value, private_key, deadline, NULL);
#endif
}

int uk_futex_wake(struct uk_thread *thread, __u32 *address, __u32 maximum,
		  bool private_key)
{
	struct uk_futex_key key = {0};
	struct uk_futex_waiter *waiter, *next;
	unsigned long flags;
	__u32 count = 0;
	int rc;

	rc = uk_futex_key_get(thread, address, private_key, false, &key);
	if (rc)
		return rc;
	futex_lock(&flags);
	uk_list_for_each_entry_safe(waiter, next, &futex_list, link) {
		if (!uk_futex_key_equal(waiter->key, &key))
			continue;
		if (waiter->is_pi) {
			rc = -EINVAL;
			goto unlock;
		}
		uk_list_del_init(&waiter->link);
		waiter->result = 0;
		uk_thread_wake(waiter->thread);
		if (++count >= maximum)
			break;
	}
	rc = count;
unlock:
	futex_unlock(flags);
	uk_futex_key_put(&key);
	return rc;
}

static int futex_requeue(__u32 *address, __u32 maximum, __u32 moves,
			 __u32 *target, const __u32 *expected, bool private_key)
{
	struct uk_thread *thread = uk_thread_current();
	struct uk_futex_call *call =
		uk_futex_call_create(thread, uk_alloc_get_default());
	struct uk_futex_call *retired = NULL;
	struct uk_futex_waiter *waiter, *next;
	unsigned long flags;
	__u32 observed, woken = 0, requeued = 0;
	int rc;

	if (!call)
		return -ENOMEM;
	for (;;) {
		uk_futex_key_put(&call->keys[0]);
		uk_futex_key_put(&call->keys[1]);
		rc = uk_futex_key_get(thread, address, private_key, false,
				      &call->keys[0]);
		if (!rc)
			rc = uk_futex_key_get(thread, target, private_key,
					      false, &call->keys[1]);
		if (rc)
			goto out;
		if (expected) {
			rc = uk_futex_key_fault(address, false, &call->keys[0]);
			if (rc)
				goto out;
		}
		futex_lock(&flags);
		if (expected) {
			rc = uk_nofault_read32(address, &observed,
					       UK_NOFAULTF_NOPAGING);
			if (rc) {
				futex_unlock(flags);
				continue;
			}
			if (observed != *expected) {
				rc = -EAGAIN;
				goto unlock;
			}
		}
		break;
	}
	uk_list_for_each_entry_safe(waiter, next, &futex_list, link) {
		if (!uk_futex_key_equal(waiter->key, &call->keys[0]))
			continue;
		if (waiter->is_pi) {
			rc = -EINVAL;
			goto unlock;
		}
		if (woken < maximum) {
			uk_list_del_init(&waiter->link);
			waiter->result = 0;
			uk_thread_wake(waiter->thread);
			woken++;
		} else if (requeued < moves) {
			if (!uk_futex_key_equal(&call->keys[0],
						&call->keys[1])) {
				uk_futex_waiter_key_set(waiter, call, 1,
							&retired);
				uk_list_del_init(&waiter->link);
				futex_queue_insert(waiter);
			}
			requeued++;
		} else {
			break;
		}
	}
	rc = woken + requeued;
unlock:
	futex_unlock(flags);
	uk_futex_retired_release(retired);
out:
	uk_futex_call_end(thread, call);
	return rc;
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
	    ((__u64)INT64_MAX - timeout->tv_nsec) / UKARCH_NSEC_PER_SEC)
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

UK_LLSYSCALL_R_DEFINE(int, futex, uint32_t *, uaddr, int, futex_op,
		      uint32_t, val, const struct timespec *, timeout,
		      uint32_t *, uaddr2, uint32_t, val3)
{
	__nsec timeout_ns;
	struct timespec copied;
	int cmd = futex_op & FUTEX_CMD_MASK;
	bool private_key = !!(futex_op & FUTEX_PRIVATE_FLAG);
	int rc;

	if (futex_op & FUTEX_CLOCK_REALTIME && !(
		cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET ||
		cmd == FUTEX_LOCK_PI2 || cmd == FUTEX_WAIT_REQUEUE_PI))
		return -ENOSYS;
	if (timeout && (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET ||
			cmd == FUTEX_LOCK_PI || cmd == FUTEX_LOCK_PI2)) {
		if (!uk_nofault_try_read(copied, timeout))
			return -EFAULT;
		rc = futex_deadline(&copied, cmd, futex_op, &timeout_ns);
		if (rc)
			return rc;
	}
	switch (cmd) {
	case FUTEX_WAIT_BITSET:
		if (val3 != UINT32_MAX)
			return -ENOSYS;
		__fallthrough;
	case FUTEX_WAIT:
		return futex_wait(uaddr, val, private_key,
				   timeout ? &timeout_ns : NULL);
	case FUTEX_WAKE:
		return uk_futex_wake(uk_thread_current(), uaddr, val,
				     private_key);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	case FUTEX_TRYLOCK_PI:
	case FUTEX_UNLOCK_PI:
		return uk_posix_futex_pi(uaddr, cmd, private_key, NULL, NULL);
	case FUTEX_LOCK_PI:
	case FUTEX_LOCK_PI2:
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	{
		struct uk_posix_signal_wait wait;

		rc = uk_posix_signal_wait_init(&wait, NULL, NULL, 0);
		if (rc)
			return rc;
		rc = uk_posix_signal_wait_check(&wait);
		if (!rc)
			rc = uk_posix_futex_pi(uaddr, cmd, private_key,
					       timeout ? &timeout_ns : NULL,
					       &wait.interrupted);
		if (rc == -EINTR)
			rc = uk_posix_signal_wait_check(&wait);
		uk_posix_signal_wait_end(&wait, rc,
					 UK_POSIX_SIGNAL_RESTART_ALWAYS);
		return rc;
	}
#else
		return uk_posix_futex_pi(uaddr, cmd, private_key,
					 timeout ? &timeout_ns : NULL, NULL);
#endif
#endif
	case FUTEX_REQUEUE:
		return futex_requeue(uaddr, val, (unsigned long)timeout,
				     uaddr2, NULL, private_key);
	case FUTEX_CMP_REQUEUE:
		return futex_requeue(uaddr, val, (unsigned long)timeout,
				     uaddr2, &val3, private_key);
	default:
		return -ENOSYS;
	}
}

static void futex_thread_cleanup(struct uk_thread *thread)
{
	struct uk_futex_waiter *waiter, *next;
	unsigned long flags;

	flags = uk_lcpu_save_irqf();
	uk_spin_lock(&futex_list_lock);
	uk_list_for_each_entry_safe(waiter, next, &futex_list, link)
		if (waiter->thread == thread)
			uk_list_del_init(&waiter->link);
	uk_spin_unlock(&futex_list_lock);
	uk_lcpu_restore_irqf(flags);
	uk_futex_call_abort_all(thread);
}

UK_THREAD_INIT_PRIO_FLAGS(0, futex_thread_cleanup, UK_PRIO_EARLIEST,
			  UK_THREAD_INITF_UKTLS);

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

static void futex_clear_child_tid(struct uk_thread *thread)
{
	pid_t *clear_ref = uk_thread_uktls_var(thread, child_tid_clear_ref);
	pid_t zero = 0;

	uk_thread_uktls_var(thread, child_tid_clear_ref) = NULL;
	if (clear_ref) {
		(void)uk_nofault_try_write(zero, clear_ref);
		uk_futex_wake(thread, (uint32_t *)clear_ref, 1, false);
	}
}

static int pthread_exit_handler(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;

	futex_clear_child_tid(event->thread);
	futex_thread_cleanup(event->thread);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTCLEANUP_HANDLER_PRIO(pthread_exit_handler, UK_PRIO_LATEST);
#endif
