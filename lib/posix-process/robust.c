/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/process.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_FUTEX
#include <linux/futex.h>
#include <uk/futex.h>
#endif
#include "process.h"

#define ROBUST_WAITERS 0x80000000U
#define ROBUST_OWNER_DIED 0x40000000U
#define ROBUST_TID_MASK 0x3fffffffU
#define ROBUST_LIST_LIMIT 2048

struct robust_head {
	__uptr next;
	long offset;
	__uptr pending;
};

UK_LLSYSCALL_R_DEFINE(int, set_robust_list, void *, head, size_t, length)
{
	if (length != sizeof(struct robust_head))
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	uk_pthread_current()->robust_list = head;
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
}

UK_LLSYSCALL_R_DEFINE(int, get_robust_list, int, tid, void **, head,
		      size_t *, length)
{
	struct posix_thread *thread;
	void *value;
	size_t size = sizeof(struct robust_head);

	uk_mutex_lock(&pprocess_mutex);
	thread = tid ? tid2pthread(tid) : uk_pthread_current();
	if (!thread) {
		uk_mutex_unlock(&pprocess_mutex);
		return -ESRCH;
	}
	value = thread->robust_list;
	uk_mutex_unlock(&pprocess_mutex);
	if (!uk_nofault_try_write(size, length) ||
	    !uk_nofault_try_write(value, head))
		return -EFAULT;
	return 0;
}

static int robust_owner_died(struct uk_thread *thread, __uptr entry,
			     long offset, pid_t tid)
{
	__u32 *address = (__u32 *)((entry & ~1UL) + offset);
	__u32 observed, expected, desired;
	int result;

#if !CONFIG_LIBPOSIX_FUTEX
	(void)thread;
#endif
	if ((__uptr)address % sizeof(*address) ||
	    !uk_nofault_try_read(observed, address))
		return -EFAULT;
#if CONFIG_LIBPOSIX_FUTEX
	if (entry & 1)
		return pprocess_pi_owner_died(thread, address, tid);
#endif
	for (;;) {
		if ((observed & ROBUST_TID_MASK) != (__u32)tid)
			return 0;
		expected = observed;
		desired = (observed & ROBUST_WAITERS) | ROBUST_OWNER_DIED;
		result = uk_nofault_cmpxchg32(address, &observed, desired, 0);
		if (result)
			return result;
		if (observed == expected)
			break;
	}
#if CONFIG_LIBPOSIX_FUTEX
	if ((observed & ROBUST_WAITERS) && !(entry & 1))
		uk_futex_wake(thread, address, 1, false);
#endif
	return 0;
}

static int robust_exit(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct posix_thread *thread;
	struct robust_head *address;
	struct robust_head head;
	__uptr entry, next;
	unsigned int count;

	uk_mutex_lock(&pprocess_mutex);
	thread = uk_thread_uktls_var(event->thread, pthread_self);
	address = thread ? thread->robust_list : NULL;
	if (thread)
		thread->robust_list = NULL;
	uk_mutex_unlock(&pprocess_mutex);
	if (!address)
		return UK_EVENT_NOT_HANDLED;
	if (!uk_nofault_try_read(head, address))
		return UK_EVENT_HANDLED_CONT;
	entry = head.next;
	for (count = 0; count < ROBUST_LIST_LIMIT &&
	     (entry & ~1UL) != (__uptr)address; count++) {
		if (!uk_nofault_try_read(next, (void *)(entry & ~1UL)))
			break;
		if ((entry & ~1UL) != (head.pending & ~1UL) &&
		    robust_owner_died(event->thread, entry, head.offset,
				      event->tid))
			break;
		entry = next;
	}
	if (head.pending)
		robust_owner_died(event->thread, head.pending, head.offset,
				  event->tid);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTCLEANUP_HANDLER_PRIO(robust_exit, UK_PRIO_EARLIEST);
