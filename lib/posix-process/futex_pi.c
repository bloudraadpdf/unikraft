/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <linux/futex.h>
#include <uk/futex.h>
#include <uk/alloc.h>
#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/plat/time.h>
#include <uk/sched.h>
#include "process.h"

static UK_LIST_HEAD(pi_waiters);

static unsigned int pi_priority(const struct uk_futex_waiter *waiter)
{
	return uk_load_n(&waiter->task->thread->effective_priority);
}

void pprocess_pi_update(struct uk_thread *changed)
{
	struct uk_futex_waiter *waiter;
	unsigned int priority;
	bool updated;

	if (changed)
		uk_store_n(&changed->effective_priority, changed->sched_priority);
	uk_list_for_each_entry(waiter, &pi_waiters, pi_link) {
		uk_store_n(&waiter->task->thread->effective_priority,
			   waiter->task->thread->sched_priority);
		uk_store_n(&waiter->owner->thread->effective_priority,
			   waiter->owner->thread->sched_priority);
	}
	do {
		updated = false;
		uk_list_for_each_entry(waiter, &pi_waiters, pi_link) {
			priority = pi_priority(waiter);
			if (priority > uk_load_n(
					&waiter->owner->thread->effective_priority)) {
				uk_store_n(&waiter->owner->thread->effective_priority,
					   priority);
				updated = true;
			}
		}
	} while (updated);
}

static struct uk_futex_waiter *pi_top(const struct uk_futex_key *key)
{
	struct uk_futex_waiter *waiter, *top = NULL;

	uk_list_for_each_entry(waiter, &pi_waiters, pi_link) {
		if (uk_futex_key_equal(waiter->key, key) &&
		    (!top || pi_priority(waiter) > pi_priority(top)))
			top = waiter;
	}
	return top;
}

void uk_posix_futex_lock(void)
{
	uk_mutex_lock(&pprocess_mutex);
}

void uk_posix_futex_unlock(void)
{
	uk_mutex_unlock(&pprocess_mutex);
}

static bool pi_cycle(struct posix_thread *thread, struct posix_thread *owner)
{
	struct uk_futex_waiter *waiter;

	for (;;) {
		if (owner == thread)
			return true;
		uk_list_for_each_entry(waiter, &pi_waiters, pi_link) {
			if (waiter->task == owner) {
				owner = waiter->owner;
				goto next;
			}
		}
		return false;
next:;
	}
}

static void pi_remove(struct uk_futex_waiter *waiter)
{
	uk_futex_queue_remove(waiter);
	uk_list_del_init(&waiter->pi_link);
}

static int pi_handover(const struct uk_futex_key *key, __u32 *address,
		       __u32 value, bool died)
{
	struct uk_futex_waiter *waiter, *top = pi_top(key);
	__u32 expected = value, desired = 0;
	int result;

	if (top && (__u32)top->owner->tid != (value & FUTEX_TID_MASK) &&
	    !(died && !(value & FUTEX_TID_MASK) && (value & FUTEX_OWNER_DIED)))
		return -EINVAL;
	if (top)
		desired = top->task->tid | FUTEX_WAITERS;
	if (died)
		desired |= FUTEX_OWNER_DIED;
	result = uk_nofault_cmpxchg32(address, &expected, desired,
				      UK_NOFAULTF_NOPAGING);
	if (result)
		return result;
	if (expected != value)
		return -EINVAL;
	if (!top)
		return 0;
	pi_remove(top);
	top->result = 0;
	uk_list_for_each_entry(waiter, &pi_waiters, pi_link) {
		if (uk_futex_key_equal(waiter->key, key))
			waiter->owner = top->task;
	}
	uk_thread_wake(top->thread);
	return 0;
}

static int pi_word_read(struct uk_thread *thread, __u32 *address, __u32 *value)
{
	struct uk_futex_key preview = {0};
	int rc;

	rc = uk_futex_key_get(thread, address, true, false, &preview);
	if (rc)
		return rc;
	for (;;) {
		rc = uk_futex_key_fault(address, false, &preview);
		if (rc)
			break;
		rc = uk_nofault_read32(address, value, UK_NOFAULTF_NOPAGING);
		if (rc != -EFAULT)
			break;
	}
	uk_futex_key_put(&preview);
	return rc;
}

static int pi_key_prepare(struct uk_thread *thread, __u32 *address,
			  int command, bool private_key, pid_t tid,
			  struct uk_futex_key *key)
{
	__u32 value;
	int rc;

	if (private_key || command == FUTEX_UNLOCK_PI) {
		rc = pi_word_read(thread, address, &value);
		if (rc)
			return rc;
		if (command == FUTEX_UNLOCK_PI) {
			if ((value & FUTEX_TID_MASK) != (__u32)tid)
				return -EPERM;
		} else if ((value & FUTEX_TID_MASK) == (__u32)tid) {
			return -EDEADLK;
		}
	}
	return uk_futex_key_get(thread, address, private_key, true, key);
}

int pprocess_pi_owner_died(struct uk_thread *thread, __u32 *address, pid_t tid)
{
	struct uk_futex_key key = {0};
	__u32 value;
	int rc;

	if ((__uptr)address % sizeof(*address))
		return -EFAULT;
	for (;;) {
		rc = pi_word_read(thread, address, &value);
		if (rc || (value & FUTEX_TID_MASK) != (__u32)tid)
			break;
		rc = uk_futex_key_get(thread, address, false, true, &key);
		if (rc)
			break;
		uk_mutex_lock(&pprocess_mutex);
		rc = uk_nofault_read32(address, &value, UK_NOFAULTF_NOPAGING);
		if (!rc && (value & FUTEX_TID_MASK) == (__u32)tid)
			rc = pi_handover(&key, address, value, true);
		pprocess_pi_update(NULL);
		uk_mutex_unlock(&pprocess_mutex);
		if (rc == -EFAULT) {
			rc = uk_futex_key_fault(address, true, &key);
		} else {
			uk_futex_key_put(&key);
			break;
		}
		uk_futex_key_put(&key);
		if (rc)
			break;
	}
	return rc;
}

int uk_posix_futex_pi(__u32 *address, int command, bool private_key,
		      const __nsec *deadline,
		      const unsigned int *interrupted)
{
	struct uk_thread *thread = uk_thread_current();
	struct posix_thread *current = uk_pthread_current(), *owner;
	struct uk_futex_call *call = NULL, *retired = NULL;
	struct uk_futex_key query = {0}, *key = &query;
	struct uk_futex_waiter *top, *waiter;
	__u32 value, expected, desired;
	bool write_fault;
	int rc;

	if (!current)
		return -ESRCH;
	if ((__uptr)address % sizeof(*address))
		return -EINVAL;
prepare:
	uk_futex_key_put(key);
	rc = pi_key_prepare(thread, address, command, private_key,
			    current->tid, key);
	if (rc)
		goto finish;
	if (!call && (command == FUTEX_LOCK_PI || command == FUTEX_LOCK_PI2)) {
		call = uk_futex_call_create(thread, uk_alloc_get_default());
		if (!call) {
			rc = -ENOMEM;
			goto finish;
		}
		call->keys[0] = query;
		query = (struct uk_futex_key){0};
		key = &call->keys[0];
	}
	rc = uk_futex_key_fault(address, false, key);
	if (rc)
		goto finish;
	uk_mutex_lock(&pprocess_mutex);
retry:
	rc = uk_nofault_read32(address, &value, UK_NOFAULTF_NOPAGING);
	if (rc) {
		write_fault = false;
		goto fault;
	}
	if (command == FUTEX_UNLOCK_PI) {
		if ((value & FUTEX_TID_MASK) != (__u32)current->tid) {
			rc = -EPERM;
			goto unlock;
		}
		if (uk_futex_pi_conflict(key)) {
			rc = -EINVAL;
			goto unlock;
		}
		rc = pi_handover(key, address, value, false);
		if (rc == -EFAULT) {
			write_fault = true;
			goto fault;
		}
		pprocess_pi_update(thread);
		goto unlock;
	}
	if ((value & FUTEX_TID_MASK) == (__u32)current->tid) {
		rc = -EDEADLK;
		goto unlock;
	}
	if (uk_futex_pi_conflict(key)) {
		rc = -EINVAL;
		goto unlock;
	}
	expected = value;
	if (!(value & FUTEX_TID_MASK))
		desired = current->tid | (value & FUTEX_OWNER_DIED);
	else
		desired = value | FUTEX_WAITERS;
	rc = uk_nofault_cmpxchg32(address, &value, desired,
				  UK_NOFAULTF_NOPAGING);
	if (rc) {
		write_fault = true;
		goto fault;
	}
	if (value != expected)
		goto retry;
	if (!(value & FUTEX_TID_MASK))
		goto unlock;
	top = pi_top(key);
	if (top && (__u32)top->owner->tid != (value & FUTEX_TID_MASK)) {
		rc = -EINVAL;
		goto unlock;
	}
	owner = tid2pthread(value & FUTEX_TID_MASK);
	if (!owner || owner->cleanup_started ||
	    owner->state == POSIX_THREAD_EXITED ||
	    owner->state == POSIX_THREAD_KILLED) {
		rc = -ESRCH;
		goto unlock;
	}
	if (command == FUTEX_TRYLOCK_PI) {
		rc = -EAGAIN;
		goto unlock;
	}
	if (pi_cycle(current, owner)) {
		rc = -EDEADLK;
		goto unlock;
	}
	if (deadline && *deadline <= ukplat_monotonic_clock()) {
		rc = -ETIMEDOUT;
		goto unlock;
	}
	waiter = &call->waiter;
	waiter->task = current;
	waiter->owner = owner;
	waiter->address = address;
	waiter->result = -EINTR;
	uk_futex_waiter_key_set(waiter, call, 0, &retired);
	waiter->is_pi = true;
	uk_futex_queue_add(waiter);
	uk_list_add_tail(&waiter->pi_link, &pi_waiters);
	pprocess_pi_update(NULL);
	if (deadline)
		uk_thread_block_until(thread, *deadline);
	else
		uk_thread_block(thread);
	uk_mutex_unlock(&pprocess_mutex);
	uk_futex_retired_release(retired);
	if ((interrupted && uk_load_n(interrupted)) ||
	    uk_posix_process_exit_requested())
		uk_thread_wake(thread);
	uk_sched_yield();
	uk_mutex_lock(&pprocess_mutex);
	rc = waiter->result;
	if (!uk_list_empty(&waiter->pi_link)) {
		pi_remove(waiter);
		if (deadline && *deadline <= ukplat_monotonic_clock())
			rc = -ETIMEDOUT;
		pprocess_pi_update(waiter->owner->thread);
	}
unlock:
	uk_mutex_unlock(&pprocess_mutex);
finish:
	if (call)
		uk_futex_call_end(thread, call);
	else
		uk_futex_key_put(key);
	if (!rc && command == FUTEX_UNLOCK_PI)
		uk_sched_yield();
	return rc;
fault:
	uk_mutex_unlock(&pprocess_mutex);
	rc = uk_futex_key_fault(address, write_fault, key);
	if (!rc)
		goto prepare;
	goto finish;
}

static bool pi_exit_step(pid_t tid)
{
	struct uk_futex_waiter *waiter = NULL, *next;
	struct uk_futex_call *pin, *retired = NULL;
	const struct uk_futex_key *key;
	__u32 *address, value;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry(next, &pi_waiters, pi_link) {
		if (next->owner->tid == tid) {
			waiter = next;
			break;
		}
	}
	if (!waiter) {
		uk_mutex_unlock(&pprocess_mutex);
		return false;
	}
	pin = waiter->key_owner;
	uk_futex_call_get(pin);
	key = waiter->key;
	address = waiter->address;
	rc = uk_nofault_read32(address, &value, UK_NOFAULTF_NOPAGING);
	if (!rc)
		rc = pi_handover(key, address, value, true);
	if (rc == -EFAULT) {
		uk_mutex_unlock(&pprocess_mutex);
		rc = uk_futex_key_recheck(address, true, key);
		uk_mutex_lock(&pprocess_mutex);
		waiter = pi_top(key);
		if (waiter && waiter->owner->tid != tid)
			waiter = NULL;
	}
	if (rc && waiter) {
		waiter->result = rc;
		pi_remove(waiter);
		uk_thread_wake(waiter->thread);
	}
	pprocess_pi_update(NULL);
	uk_mutex_unlock(&pprocess_mutex);
	uk_futex_call_put(pin, &retired);
	uk_futex_retired_release(retired);
	return true;
}

static int pi_exit(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct uk_futex_waiter *waiter, *next;

	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry_safe(waiter, next, &pi_waiters, pi_link) {
		if (waiter->task->tid == event->tid) {
			struct uk_thread *owner = waiter->owner->thread;

			pi_remove(waiter);
			pprocess_pi_update(owner);
		}
	}
	uk_mutex_unlock(&pprocess_mutex);
	while (pi_exit_step(event->tid))
		;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_pi_update(event->thread);
	uk_mutex_unlock(&pprocess_mutex);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTCLEANUP_HANDLER_PRIO(pi_exit, UK_PRIO_AFTER(UK_PRIO_EARLIEST));
