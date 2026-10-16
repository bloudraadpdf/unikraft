/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/process.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_FUTEX
#include <linux/futex.h>
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
	uk_pthread_current()->robust_list = head;
	return 0;
}

UK_LLSYSCALL_R_DEFINE(int, get_robust_list, int, tid, void **, head,
		      size_t *, length)
{
	struct posix_thread *thread;
	void *value;
	size_t size = sizeof(struct robust_head);
	int result = 0;

	uk_mutex_lock(&pprocess_mutex);
	thread = tid ? tid2pthread(tid) : uk_pthread_current();
	if (!thread) {
		result = -ESRCH;
		goto out;
	}
	value = thread->robust_list;
	if (!uk_nofault_try_write(size, length) ||
	    !uk_nofault_try_write(value, head))
		result = -EFAULT;
out:
	uk_mutex_unlock(&pprocess_mutex);
	return result;
}

static int robust_owner_died(__uptr entry, long offset, pid_t tid)
{
	__u32 *address = (__u32 *)((entry & ~1UL) + offset);
	__u32 observed, expected, desired;
	int result;

	if ((__uptr)address % sizeof(*address) ||
	    !uk_nofault_try_read(observed, address))
		return -EFAULT;
	for (;;) {
		if ((observed & ROBUST_TID_MASK) != (__u32)tid)
			return 0;
		expected = observed;
		desired = (observed & ROBUST_WAITERS) | ROBUST_OWNER_DIED;
		result = uk_nofault_cmpxchg32(address, &observed, desired);
		if (result)
			return result;
		if (observed == expected)
			break;
	}
#if CONFIG_LIBPOSIX_FUTEX
	if (observed & ROBUST_WAITERS)
		uk_syscall_r_futex((long)address, FUTEX_WAKE, 1, 0, 0, 0);
#endif
	return 0;
}

static int robust_exit(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct posix_thread *thread = tid2pthread(event->tid);
	struct robust_head *address;
	struct robust_head head;
	__uptr entry, next;
	unsigned int count;

	if (!thread || !thread->robust_list)
		return UK_EVENT_NOT_HANDLED;
	address = thread->robust_list;
	thread->robust_list = NULL;
	if (!uk_nofault_try_read(head, address))
		return UK_EVENT_HANDLED_CONT;
	entry = head.next;
	for (count = 0; count < ROBUST_LIST_LIMIT &&
	     (entry & ~1UL) != (__uptr)address; count++) {
		if (!uk_nofault_try_read(next, (void *)(entry & ~1UL)))
			break;
		if ((entry & ~1UL) != (head.pending & ~1UL) &&
		    robust_owner_died(entry, head.offset, event->tid))
			break;
		entry = next;
	}
	if (head.pending)
		robust_owner_died(head.pending, head.offset, event->tid);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_PTEXIT_HANDLER_PRIO(robust_exit, UK_PRIO_EARLIEST);
