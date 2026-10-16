/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <linux/futex.h>
#include <uk/futex.h>
#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/plat/time.h>
#include <uk/sched.h>
#include "process.h"

struct pi_waiter {
	struct uk_list_head link;
	__u32 *address;
	struct posix_thread *thread;
	struct posix_thread *owner;
	int result;
};

static UK_LIST_HEAD(pi_waiters);

void pprocess_pi_update(struct uk_thread *changed)
{
	struct pi_waiter *waiter;
	unsigned int priority;
	bool updated;

	if (changed)
		uk_store_n(&changed->effective_priority, changed->sched_priority);
	uk_list_for_each_entry(waiter, &pi_waiters, link) {
		uk_store_n(&waiter->thread->thread->effective_priority,
			   waiter->thread->thread->sched_priority);
		uk_store_n(&waiter->owner->thread->effective_priority,
			   waiter->owner->thread->sched_priority);
	}
	do {
		updated = false;
		uk_list_for_each_entry(waiter, &pi_waiters, link) {
			priority = uk_load_n(
				&waiter->thread->thread->effective_priority);
			if (priority > uk_load_n(
					&waiter->owner->thread->effective_priority)) {
				uk_store_n(&waiter->owner->thread->effective_priority,
					   priority);
				updated = true;
			}
		}
	} while (updated);
}

static struct pi_waiter *pi_top(__u32 *address)
{
	struct pi_waiter *waiter, *top = NULL;

	uk_list_for_each_entry(waiter, &pi_waiters, link) {
		if (waiter->address == address &&
		    (!top || uk_load_n(&waiter->thread->thread->effective_priority) >
		     uk_load_n(&top->thread->thread->effective_priority)))
			top = waiter;
	}
	return top;
}

int uk_posix_futex_lock(__u32 *address, __u32 *second)
{
	uk_mutex_lock(&pprocess_mutex);
	if (pi_top(address) || (second && pi_top(second))) {
		uk_mutex_unlock(&pprocess_mutex);
		return -EINVAL;
	}
	return 0;
}

void uk_posix_futex_unlock(void)
{
	uk_mutex_unlock(&pprocess_mutex);
}

static bool pi_cycle(struct posix_thread *thread, struct posix_thread *owner)
{
	struct pi_waiter *waiter;

	for (;;) {
		if (owner == thread)
			return true;
		uk_list_for_each_entry(waiter, &pi_waiters, link) {
			if (waiter->thread == owner) {
				owner = waiter->owner;
				goto next;
			}
		}
		return false;
next:;
	}
}

static int pi_handover(__u32 *address, __u32 value, bool died)
{
	struct pi_waiter *waiter, *top = pi_top(address);
	__u32 expected = value, desired = 0;
	int result;

	if (top && (__u32)top->owner->tid != (value & FUTEX_TID_MASK) &&
	    !(died && !(value & FUTEX_TID_MASK) && (value & FUTEX_OWNER_DIED)))
		return -EINVAL;
	if (top)
		desired = top->thread->tid | FUTEX_WAITERS;
	if (died)
		desired |= FUTEX_OWNER_DIED;
	result = uk_nofault_cmpxchg32(address, &expected, desired);
	if (result)
		return result;
	if (expected != value)
		return -EINVAL;
	if (!top)
		return 0;
	uk_list_del_init(&top->link);
	top->result = 0;
	uk_list_for_each_entry(waiter, &pi_waiters, link) {
		if (waiter->address == address)
			waiter->owner = top->thread;
	}
	uk_thread_wake(top->thread->thread);
	return 0;
}

int uk_posix_futex_pi(__u32 *address, int command, const __nsec *deadline,
		       const unsigned int *interrupted)
{
	struct posix_thread *current = uk_pthread_current(), *owner;
	struct pi_waiter *top;
	struct pi_waiter waiter = {
		.address = address, .thread = current, .result = -EINTR
	};
	__u32 value, expected, desired;
	int result;

	if ((__uptr)address % sizeof(*address))
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
retry:
	if (uk_futex_has_waiters(address)) {
		result = -EINVAL;
		goto out;
	}
	if (!uk_nofault_try_read(value, address)) {
		result = -EFAULT;
		goto out;
	}
	if (command == FUTEX_UNLOCK_PI) {
		if ((value & FUTEX_TID_MASK) != (__u32)current->tid) {
			result = -EPERM;
			goto out;
		}
		result = pi_handover(address, value, false);
		pprocess_pi_update(current->thread);
		goto out;
	}
	if ((value & FUTEX_TID_MASK) == (__u32)current->tid) {
		result = -EDEADLK;
		goto out;
	}
	expected = value;
	if (!(value & FUTEX_TID_MASK))
		desired = current->tid | (value & FUTEX_OWNER_DIED);
	else
		desired = value | FUTEX_WAITERS;
	result = uk_nofault_cmpxchg32(address, &value, desired);
	if (result)
		goto out;
	if (value != expected)
		goto retry;
	if (!(value & FUTEX_TID_MASK)) {
		result = 0;
		goto out;
	}
	top = pi_top(address);
	if (top && (__u32)top->owner->tid != (value & FUTEX_TID_MASK)) {
		result = -EINVAL;
		goto out;
	}
	owner = tid2pthread(value & FUTEX_TID_MASK);
	if (!owner || owner->state == POSIX_THREAD_EXITED ||
	    owner->state == POSIX_THREAD_KILLED) {
		result = -ESRCH;
		goto out;
	}
	if (command == FUTEX_TRYLOCK_PI) {
		result = -EAGAIN;
		goto out;
	}
	if (pi_cycle(current, owner)) {
		result = -EDEADLK;
		goto out;
	}
	if (deadline && *deadline <= ukplat_monotonic_clock()) {
		result = -ETIMEDOUT;
		goto out;
	}
	waiter.owner = owner;
	uk_list_add_tail(&waiter.link, &pi_waiters);
	pprocess_pi_update(NULL);
	if (deadline)
		uk_thread_block_until(current->thread, *deadline);
	else
		uk_thread_block(current->thread);
	uk_mutex_unlock(&pprocess_mutex);
	if ((interrupted && uk_load_n(interrupted)) ||
	    uk_posix_process_exit_requested())
		uk_thread_wake(current->thread);
	uk_sched_yield();
	uk_mutex_lock(&pprocess_mutex);
	result = waiter.result;
	if (!uk_list_empty(&waiter.link)) {
		uk_list_del_init(&waiter.link);
		if (deadline && *deadline <= ukplat_monotonic_clock())
			result = -ETIMEDOUT;
		pprocess_pi_update(waiter.owner->thread);
	}
out:
	uk_mutex_unlock(&pprocess_mutex);
	if (!result && command == FUTEX_UNLOCK_PI)
		uk_sched_yield();
	return result;
}

static int pi_exit(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct pi_waiter *waiter, *next;
	__u32 value;
	int result;

	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry_safe(waiter, next, &pi_waiters, link) {
		if (waiter->thread->tid == event->tid) {
			struct uk_thread *owner = waiter->owner->thread;

			uk_list_del_init(&waiter->link);
			pprocess_pi_update(owner);
		}
	}
	for (;;) {
		waiter = NULL;
		uk_list_for_each_entry(next, &pi_waiters, link) {
			if (next->owner->tid == event->tid) {
				waiter = next;
				break;
			}
		}
		if (!waiter)
			break;
		result = uk_nofault_try_read(value, waiter->address) ?
			pi_handover(waiter->address, value, true) : -EFAULT;
		if (result) {
			waiter->result = result;
			uk_list_del_init(&waiter->link);
			uk_thread_wake(waiter->thread->thread);
		}
	}
	pprocess_pi_update(event->thread);
	uk_mutex_unlock(&pprocess_mutex);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTEXIT_HANDLER_PRIO(pi_exit, UK_PRIO_AFTER(UK_PRIO_EARLIEST));
