/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/syscall.h>
#include <uk/sched.h>
#include "process.h"
#include "scheduling.h"

#define SCHED_OTHER 0
#define SCHED_RR 2

struct sched_param {
	int sched_priority;
};

#if !CONFIG_LIBPOSIX_FUTEX
void pprocess_pi_update(struct uk_thread *thread)
{
	uk_store_n(&thread->effective_priority, thread->sched_priority);
}
#endif

static int schedule_update(pid_t tid, int policy, const struct sched_param *param)
{
	struct posix_thread *thread;
	struct sched_param value;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *caller = NULL, *credentials = NULL;
#endif
	int result = 0;

	if (tid < 0)
		return -EINVAL;
	if (!uk_nofault_try_read(value, param))
		return -EFAULT;
	uk_mutex_lock(&pprocess_mutex);
	thread = tid ? tid2pthread(tid) : uk_pthread_current();
	if (!thread) {
		result = -ESRCH;
		goto out;
	}
	if (policy == -1)
		policy = thread->thread->sched_policy;
	if (policy != SCHED_OTHER && policy != SCHED_RR) {
		result = policy == 1 ? -EOPNOTSUPP : -EINVAL;
		goto out;
	}
	if ((policy == SCHED_OTHER && value.sched_priority) ||
	    (policy == SCHED_RR && (value.sched_priority < 1 ||
				 value.sched_priority > 99))) {
		result = -EINVAL;
		goto out;
	}
#if CONFIG_LIBPOSIX_USER
	result = pprocess_schedule_authorize(thread->thread, &caller,
					     &credentials);
	if (result)
		goto out;
	if (policy == SCHED_RR &&
	    !(pu_credentials_privileges(caller)->effective & PU_CAP_SYS_NICE)) {
		rlim_t limit = thread->process->limits[RLIMIT_RTPRIO].rlim_cur;
		int previous = thread->thread->sched_policy == SCHED_RR ?
			thread->thread->sched_priority : 0;

		if (value.sched_priority > previous &&
		    (rlim_t)value.sched_priority > limit) {
			result = -EPERM;
			goto out;
		}
	}
#endif
	thread->thread->sched_policy = policy;
	thread->thread->sched_priority = value.sched_priority;
	pprocess_pi_update(thread->thread);
out:
	uk_mutex_unlock(&pprocess_mutex);
#if CONFIG_LIBPOSIX_USER
	if (credentials)
		pu_credentials_release(credentials);
	if (caller)
		pu_credentials_release(caller);
#endif
	if (!result)
		uk_sched_yield();
	return result;
}

UK_LLSYSCALL_R_DEFINE(int, sched_setscheduler, pid_t, tid, int, policy,
		      const struct sched_param *, param)
{
	if (policy < 0)
		return -EINVAL;
	return schedule_update(tid, policy, param);
}

UK_LLSYSCALL_R_DEFINE(int, sched_setparam, pid_t, tid,
		      const struct sched_param *, param)
{
	return schedule_update(tid, -1, param);
}

UK_LLSYSCALL_R_DEFINE(int, sched_getscheduler, pid_t, tid)
{
	struct posix_thread *thread;
	int result;

	if (tid < 0)
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	thread = tid ? tid2pthread(tid) : uk_pthread_current();
	result = thread ? thread->thread->sched_policy : -ESRCH;
	uk_mutex_unlock(&pprocess_mutex);
	return result;
}

UK_LLSYSCALL_R_DEFINE(int, sched_getparam, pid_t, tid, struct sched_param *, param)
{
	struct posix_thread *thread;
	struct sched_param value;
	int result = 0;

	if (tid < 0)
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	thread = tid ? tid2pthread(tid) : uk_pthread_current();
	if (!thread) {
		result = -ESRCH;
		goto out;
	}
	value.sched_priority = thread->thread->sched_priority;
	if (!uk_nofault_try_write(value, param))
		result = -EFAULT;
out:
	uk_mutex_unlock(&pprocess_mutex);
	return result;
}

UK_LLSYSCALL_R_DEFINE(int, sched_get_priority_min, int, policy)
{
	return policy == SCHED_OTHER ? 0 : policy == SCHED_RR ? 1 : -EINVAL;
}

UK_LLSYSCALL_R_DEFINE(int, sched_get_priority_max, int, policy)
{
	return policy == SCHED_OTHER ? 0 : policy == SCHED_RR ? 99 : -EINVAL;
}

static int scheduling_clone(void *arg)
{
	struct posix_process_clone_event_data *event = arg;

	if (!event->parent)
		return UK_EVENT_NOT_HANDLED;
	event->child->sched_policy = event->parent->sched_policy;
	event->child->sched_priority = event->parent->sched_priority;
	uk_store_n(&event->child->effective_priority,
		   event->child->sched_priority);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_CLONE_HANDLER(0, scheduling_clone);
