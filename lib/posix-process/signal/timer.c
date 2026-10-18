/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <uk/init.h>
#include <uk/nofault.h>
#include <uk/plat/time.h>
#include <uk/posix-time.h>
#include <uk/posix-process-timer.h>
#include <uk/sched.h>
#include "timer.h"

#define TIMER_NSEC 1000000000ULL
#define TIMER_LIMIT ((__u64)INT64_MAX)
#define TIMER_THREAD_ID 4

struct timer_event {
	__u64 value;
	__s32 signum, notify, tid;
	__u8 padding[44];
};

UK_CTASSERT(sizeof(struct timer_event) == 64);

static UK_LIST_HEAD(timer_contexts);
static DEFINE_WAIT_QUEUE(timer_wait);
static bool service_ready, service_stopping, service_done;

static __u64 timer_add(__u64 first, __u64 second)
{
	return second > TIMER_LIMIT - first ? TIMER_LIMIT : first + second;
}

static bool timer_spec_valid(const struct timespec *value)
{
	return value->tv_sec >= 0 && value->tv_nsec >= 0 &&
	       value->tv_nsec < (long)TIMER_NSEC;
}

static __u64 timer_nsec(const struct timespec *value)
{
	return (__u64)value->tv_sec >
			       (TIMER_LIMIT - value->tv_nsec) / TIMER_NSEC
		       ? TIMER_LIMIT
		       : (__u64)value->tv_sec * TIMER_NSEC + value->tv_nsec;
}

static struct timespec timer_spec(__u64 value)
{
	return (struct timespec){.tv_sec = value / TIMER_NSEC,
				 .tv_nsec = value % TIMER_NSEC};
}

static __u64 timer_clock(clockid_t clock)
{
	struct timespec value;
	int rc = uk_sys_clock_gettime(clock, &value);

	UK_BUGON(rc);
	return timer_nsec(&value);
}

void pprocess_timer_set_locked(struct posix_timer *timer, int flags,
			       const struct itimerspec *value, __u64 realtime,
			       __u64 monotonic)
{
	__u64 duration = timer_nsec(&value->it_value);

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	timer->notification_current = false;
	timer->waiting_signal = false;
	timer->overrun = 0;
	timer->interval = duration || timer->real_interval
				  ? timer_nsec(&value->it_interval)
				  : 0;
	timer->armed = duration != 0;
	if (!duration)
		return;
	timer->effective_clock =
		timer->clock == CLOCK_REALTIME && (flags & TIMER_ABSTIME)
			? CLOCK_REALTIME
			: CLOCK_MONOTONIC;
	timer->deadline =
		flags & TIMER_ABSTIME
			? duration
			: timer_add(timer->effective_clock == CLOCK_REALTIME
					    ? realtime
					    : monotonic,
				    duration);
}

int pprocess_timer_advance_locked(struct posix_timer *timer, __u64 now,
				  enum uk_signal_dequeue_reason reason)
{
	__u64 periods, overruns;
	int result;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (!timer->armed)
		return 0;
	periods = timer->interval && now >= timer->deadline
			  ? 1 + (now - timer->deadline) / timer->interval
			  : 1;
	overruns = periods - 1;
	result = overruns > INT_MAX ? INT_MAX : (int)overruns;
	if (reason == UK_SIGNAL_CONSUME)
		timer->overrun = result;
	if (timer->interval) {
		if (periods >
		    (TIMER_LIMIT - timer->deadline) / timer->interval) {
			timer->deadline = TIMER_LIMIT;
			timer->armed = now < TIMER_LIMIT;
		} else {
			timer->deadline += periods * timer->interval;
		}
	} else {
		timer->armed = false;
	}
	timer->waiting_signal = false;
	return result;
}

void pprocess_timer_get_locked(struct posix_timer *timer,
			       struct itimerspec *value, __u64 realtime,
			       __u64 monotonic)
{
	__u64 now =
		timer->effective_clock == CLOCK_REALTIME ? realtime : monotonic;
	__u64 remaining = 0;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (timer->notify == SIGEV_NONE && !timer->real_interval) {
		if (timer->armed && timer->interval && now >= timer->deadline)
			pprocess_timer_advance_locked(timer, now,
						      UK_SIGNAL_DISCARD);
		if (timer->deadline > now)
			remaining = timer->deadline - now;
	} else if (timer->armed && !timer->waiting_signal) {
		remaining = timer->deadline > now ? timer->deadline - now : 1;
	}
	*value =
		(struct itimerspec){.it_value = timer_spec(remaining),
				    .it_interval = timer_spec(timer->interval)};
}

static void timer_attach_locked(struct posix_timer_context *context)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (!context->attached) {
		pprocess_acquire(context->process);
		pprocess_signal_pdesc_acquire(context->signals);
		uk_list_add_tail(&context->service_link, &timer_contexts);
		context->attached = true;
	}
	uk_waitq_wake_up(&timer_wait);
}

static void timer_notification_dequeue(struct uk_signal *signal,
				       enum uk_signal_dequeue_reason reason)
{
	struct posix_timer *timer =
		__containerof(signal, struct posix_timer, signal);
	int overrun;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	timer->queued = false;
	if (timer->notification_current && !timer->deleted) {
		overrun = pprocess_timer_advance_locked(
			timer, timer_clock(timer->effective_clock), reason);
		if (!timer->real_interval && reason == UK_SIGNAL_CONSUME)
			signal->siginfo.si_overrun = overrun;
	}
	timer->notification_settled = true;
}

static void timer_notification_release(struct uk_signal *signal)
{
	struct posix_timer *timer =
		__containerof(signal, struct posix_timer, signal);

	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(timer->loaned && !timer->queued);
	if (timer->notification_current && !timer->notification_settled &&
	    !timer->deleted && !timer->waiting_signal)
		pprocess_timer_advance_locked(timer,
					      timer_clock(
						      timer->effective_clock),
					      UK_SIGNAL_DISCARD);
	timer->loaned = false;
	uk_waitq_wake_up(&timer_wait);
	uk_mutex_unlock(&pprocess_mutex);
}

static const struct uk_signal_ops timer_signal_ops = {
	.bypass_limit = true,
	.reserved_quota = true,
	.dequeue_locked = timer_notification_dequeue,
	.release = timer_notification_release,
};

static const struct uk_signal_ops real_signal_ops = {
	.coalesce_standard = true,
	.bypass_limit = true,
	.dequeue_locked = timer_notification_dequeue,
	.release = timer_notification_release,
};

void pprocess_timers_init(struct posix_process *process)
{
	struct posix_timer_context *context = &process->signal->timers;

	context->process = process;
	context->signals = process->signal;
	UK_INIT_LIST_HEAD(&context->service_link);
	UK_INIT_LIST_HEAD(&context->active);
	UK_INIT_LIST_HEAD(&context->retired);
	context->real = (struct posix_timer){
		.context = context,
		.clock = CLOCK_REALTIME,
		.effective_clock = CLOCK_MONOTONIC,
		.real_interval = true,
		.notify = SIGEV_SIGNAL,
		.id = -1,
		.signal = {.ops = &real_signal_ops,
			   .siginfo = {.si_signo = SIGALRM,
				       .si_code = SI_KERNEL}},
	};
}

static void timer_withdraw_locked(struct posix_timer *timer,
				  struct pprocess_retired *retired)
{
	struct posix_process *process = timer->context->process;
	struct posix_thread *target = NULL;

	if (!timer->queued)
		return;
	if (timer->target) {
		target = pprocess_task_resolve_locked(process, timer->target);
		UK_ASSERT(target);
	}
	UK_BUGON(!pprocess_signal_withdraw_locked(process, target,
						  &timer->signal, retired));
}

static void timer_retire_locked(struct posix_timer *timer,
				struct pprocess_retired *retired)
{
	timer->deleted = true;
	timer->notification_current = false;
	timer->armed = false;
	timer_withdraw_locked(timer, retired);
	uk_list_del(&timer->link);
	uk_list_add_tail(&timer->link, &timer->context->retired);
	timer_attach_locked(timer->context);
}

void pprocess_timers_stop_locked(struct posix_process *process, bool all,
				 struct pprocess_retired *retired)
{
	struct posix_timer_context *context = &process->signal->timers;
	struct posix_timer *timer, *next;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	uk_list_for_each_entry_safe(timer, next, &context->active, link)
		timer_retire_locked(timer, retired);
	context->next_id = 0;
	if (all) {
		context->stopping = true;
		context->real.armed = false;
		context->real.waiting_signal = false;
		context->real.notification_current = false;
		timer_withdraw_locked(&context->real, retired);
	}
	if (context->attached)
		uk_waitq_wake_up(&timer_wait);
}

void pprocess_timers_signal_consumed_locked(struct posix_process *process,
					    int signum)
{
	struct posix_timer *timer = &process->signal->timers.real;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	if (signum == SIGALRM && timer->waiting_signal && timer->armed) {
		pprocess_timer_advance_locked(timer,
					      timer_clock(
						      timer->effective_clock),
					      UK_SIGNAL_DISCARD);
		uk_waitq_wake_up(&timer_wait);
	}
}

static bool timer_context_needed(struct posix_timer_context *context)
{
	struct posix_timer *timer;

	if (!uk_list_empty(&context->retired) || context->real.loaned ||
	    context->real.armed || context->real.waiting_signal)
		return true;
	uk_list_for_each_entry(timer, &context->active, link)
		if (timer->loaned ||
		    (timer->armed && timer->notify != SIGEV_NONE))
			return true;
	return false;
}

static __u64 timer_update_locked(struct posix_timer *timer)
{
	struct posix_process *process = timer->context->process;
	struct posix_thread *target = NULL, *recipient;
	__u64 now, monotonic;
	int rc;

	if (!timer->armed || timer->loaned || timer->waiting_signal ||
	    timer->notify == SIGEV_NONE)
		return 0;
	now = timer_clock(timer->effective_clock);
	if (now < timer->deadline) {
		monotonic = MIN(ukplat_monotonic_clock(), TIMER_LIMIT);
		return timer_add(monotonic, timer->deadline - now);
	}
	if (timer->target) {
		target = pprocess_task_resolve_locked(process, timer->target);
		if (!target) {
			timer->armed = false;
			return 0;
		}
	}
	recipient = target ? target : tid2pthread(process->pid);
	if (IS_IGNORED(process, timer->signal.siginfo.si_signo) &&
	    (!recipient ||
	     !IS_MASKED(recipient, timer->signal.siginfo.si_signo))) {
		pprocess_timer_advance_locked(timer, now, UK_SIGNAL_DISCARD);
		return timer->armed ? timer_add(MIN(ukplat_monotonic_clock(),
						    TIMER_LIMIT),
						timer->deadline - now)
				    : 0;
	}
	timer->loaned = true;
	timer->notification_current = true;
	timer->notification_settled = false;
	timer->waiting_signal = timer->real_interval;
	timer->signal.siginfo.si_overrun = 0;
	rc = pprocess_signal_enqueue(process, target, &timer->signal);
	if (rc) {
		timer->waiting_signal = false;
		uk_signal_free(&timer->signal);
	} else if (timer->loaned) {
		timer->queued = true;
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
		if (target)
			pthread_signal_files_notify(process, target,
						    timer->signal.siginfo
							    .si_signo);
		else
			pprocess_signal_files_notify(process,
						     timer->signal.siginfo
							     .si_signo);
#endif
	}
	return 0;
}

static __noreturn void timer_service(void *unused __unused)
{
	struct posix_timer_context *context;
	struct posix_timer *timer, *destroy;
	struct uk_signal_pdesc *release;
	struct posix_process *process;
	__u64 deadline, next;

	for (;;) {
		destroy = NULL;
		release = NULL;
		process = NULL;
		deadline = 0;
		uk_mutex_lock(&pprocess_mutex);
		uk_list_for_each_entry(context, &timer_contexts,
				       service_link) {
			uk_list_for_each_entry(timer, &context->retired,
					       link) {
				if (timer->loaned)
					continue;
				uk_list_del(&timer->link);
				UK_ASSERT(context->signals->reserved_count);
				context->signals->reserved_count--;
				destroy = timer;
				goto unlock;
			}
			if (!context->stopping) {
				uk_list_for_each_entry(timer, &context->active,
						       link) {
					next = timer_update_locked(timer);
					if (next &&
					    (!deadline || next < deadline))
						deadline = next;
				}
				next = timer_update_locked(&context->real);
				if (next && (!deadline || next < deadline))
					deadline = next;
			}
			if (!timer_context_needed(context)) {
				uk_list_del_init(&context->service_link);
				context->attached = false;
				release = context->signals;
				process = context->process;
				goto unlock;
			}
		}
		if (service_stopping && uk_list_empty(&timer_contexts)) {
			service_done = true;
			uk_waitq_wake_up(&timer_wait);
			uk_mutex_unlock(&pprocess_mutex);
			uk_sched_thread_exit();
		}
		uk_waitq_wait_deadline_locked(&timer_wait, deadline,
					      uk_mutex_lock, uk_mutex_unlock,
					      &pprocess_mutex);
unlock:
		uk_mutex_unlock(&pprocess_mutex);
		if (destroy) {
			struct uk_alloc *allocator =
				destroy->context->signals->allocator;

			pprocess_task_put(destroy->target);
			uk_free(allocator, destroy);
		}
		if (release) {
			pprocess_signal_pdesc_put(release);
			pprocess_put(process);
		}
	}
}

static int timer_service_init(struct uk_init_ctx *ctx __unused)
{
	struct uk_thread *worker =
		uk_sched_thread_create_fn1(uk_sched_current(), timer_service,
					   NULL, 0, 0, false, false,
					   "posix-timers", NULL, NULL);

	if (!worker)
		return -ENOMEM;
	service_ready = true;
	return 0;
}

static void timer_service_term(struct uk_term_ctx *ctx __unused)
{
	if (!service_ready)
		return;
	uk_mutex_lock(&pprocess_mutex);
	service_stopping = true;
	uk_waitq_wake_up(&timer_wait);
	while (!service_done)
		uk_waitq_wait_locked(&timer_wait, uk_mutex_lock,
				     uk_mutex_unlock, &pprocess_mutex);
	uk_mutex_unlock(&pprocess_mutex);
}

uk_late_initcall_prio(timer_service_init, timer_service_term, UK_PRIO_EARLIEST);

void uk_posix_timers_clock_changed(void)
{
	uk_mutex_lock(&pprocess_mutex);
	uk_waitq_wake_up(&timer_wait);
	uk_mutex_unlock(&pprocess_mutex);
}

static struct posix_timer *timer_find_locked(struct posix_process *process,
					     int id)
{
	struct posix_timer *timer;

	if (id < 0)
		return NULL;
	uk_list_for_each_entry(timer, &process->signal->timers.active, link)
		if (timer->id == id)
			return timer;
	return NULL;
}

static int timer_clock_admit(clockid_t clock)
{
	switch (clock) {
	case CLOCK_REALTIME:
	case CLOCK_MONOTONIC:
	case CLOCK_BOOTTIME:
		return 0;
	case CLOCK_PROCESS_CPUTIME_ID:
	case CLOCK_THREAD_CPUTIME_ID:
	case CLOCK_MONOTONIC_RAW:
	case CLOCK_REALTIME_COARSE:
	case CLOCK_MONOTONIC_COARSE:
		return -EOPNOTSUPP;
	default:
		return clock < 0 ? -EOPNOTSUPP : -EINVAL;
	}
}

int uk_posix_timer_create(clockid_t clock, const void *event_user, int *id_user)
{
	struct timer_event event = {.notify = SIGEV_SIGNAL, .signum = SIGALRM};
	struct posix_process *process = uk_pprocess_current();
	struct uk_signal_pdesc *signals = process->signal;
	struct posix_timer_context *context = &signals->timers;
	struct posix_timer *timer;
	unsigned int first, id;
	int rc;

	if (event_user &&
	    !uk_nofault_try_read(event, (const struct timer_event *)event_user))
		return -EFAULT;
	rc = timer_clock_admit(clock);
	if (rc)
		return rc;
	if (event.notify != SIGEV_SIGNAL && event.notify != SIGEV_NONE &&
	    event.notify != SIGEV_THREAD && event.notify != TIMER_THREAD_ID)
		return -EINVAL;
	if (event.notify != SIGEV_NONE && !IS_VALID(event.signum))
		return -EINVAL;
	timer = uk_zalloc(signals->allocator, sizeof(*timer));
	if (!timer)
		return -EAGAIN;
	timer->context = context;
	timer->clock = clock;
	timer->effective_clock = CLOCK_MONOTONIC;
	timer->notify = event.notify;
	timer->signal.ops = &timer_signal_ops;
	uk_mutex_lock(&pprocess_mutex);
	if (!service_ready || service_stopping || context->stopping) {
		rc = -EAGAIN;
		goto out;
	}
	if (event.notify == TIMER_THREAD_ID) {
		timer->target = pprocess_task_get_locked(process, event.tid);
		if (!timer->target) {
			rc = -EINVAL;
			goto out;
		}
	}
	if (pprocess_signal_quota_full_locked(signals)) {
		rc = -EAGAIN;
		goto out;
	}
	first = context->next_id;
	id = first;
	while (timer_find_locked(process, (int)id)) {
		id = id == INT_MAX ? 0 : id + 1;
		if (id == first) {
			rc = -EAGAIN;
			goto out;
		}
	}
	timer->id = (int)id;
	if (!uk_nofault_try_write(timer->id, id_user)) {
		rc = -EFAULT;
		goto out;
	}
	timer->signal.siginfo.si_signo = event.signum;
	timer->signal.siginfo.si_code = SI_TIMER;
	timer->signal.siginfo.si_timerid = timer->id;
	if (event_user)
		timer->signal.siginfo.si_value.sival_ptr =
			(void *)(__uptr)event.value;
	else
		timer->signal.siginfo.si_value.sival_int = timer->id;
	context->next_id = id == INT_MAX ? 0 : id + 1;
	signals->reserved_count++;
	uk_list_add_tail(&timer->link, &context->active);
	uk_mutex_unlock(&pprocess_mutex);
	return 0;
out:
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_task_put(timer->target);
	uk_free(signals->allocator, timer);
	return rc;
}

int uk_posix_timer_delete(int id)
{
	struct posix_process *process = uk_pprocess_current();
	struct pprocess_retired retired = {0};
	struct posix_timer *timer;
	int result;

	uk_mutex_lock(&pprocess_mutex);
	timer = timer_find_locked(process, id);
	if (timer)
		timer_retire_locked(timer, &retired);
	result = timer ? 0 : -EINVAL;
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	return result;
}

int uk_posix_timer_gettime(int id, struct itimerspec *output)
{
	struct posix_process *process = uk_pprocess_current();
	struct posix_timer *timer;
	struct itimerspec value;
	bool found;

	uk_mutex_lock(&pprocess_mutex);
	timer = timer_find_locked(process, id);
	if (timer)
		pprocess_timer_get_locked(timer, &value,
					  timer_clock(CLOCK_REALTIME),
					  timer_clock(CLOCK_MONOTONIC));
	found = !!timer;
	uk_mutex_unlock(&pprocess_mutex);
	if (!found)
		return -EINVAL;
	return uk_nofault_try_write(value, output) ? 0 : -EFAULT;
}

static void timer_replace_locked(struct posix_timer *timer, int flags,
				 const struct itimerspec *value,
				 struct itimerspec *old,
				 struct pprocess_retired *retired)
{
	__u64 realtime = timer_clock(CLOCK_REALTIME);
	__u64 monotonic = timer_clock(CLOCK_MONOTONIC);

	if (old)
		pprocess_timer_get_locked(timer, old, realtime, monotonic);
	pprocess_timer_set_locked(timer, flags, value, realtime, monotonic);
	timer_withdraw_locked(timer, retired);
	timer_attach_locked(timer->context);
}

int uk_posix_timer_settime(int id, int flags, const struct itimerspec *input,
			   struct itimerspec *output)
{
	struct posix_process *process = uk_pprocess_current();
	struct pprocess_retired retired = {0};
	struct posix_timer *timer;
	struct itimerspec value, old;
	bool found;

	if (!input)
		return -EINVAL;
	if (!uk_nofault_try_read(value, input))
		return -EFAULT;
	if (!timer_spec_valid(&value.it_value) ||
	    !timer_spec_valid(&value.it_interval))
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	timer = timer_find_locked(process, id);
	if (timer)
		timer_replace_locked(timer, flags, &value, output ? &old : NULL,
				     &retired);
	found = !!timer;
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	if (!found)
		return -EINVAL;
	return !output || uk_nofault_try_write(old, output) ? 0 : -EFAULT;
}

int uk_posix_timer_getoverrun(int id)
{
	struct posix_process *process = uk_pprocess_current();
	struct posix_timer *timer;
	int result;

	uk_mutex_lock(&pprocess_mutex);
	timer = timer_find_locked(process, id);
	result = timer ? timer->overrun : -EINVAL;
	uk_mutex_unlock(&pprocess_mutex);
	return result;
}

static struct itimerval timer_timeval(const struct itimerspec *value)
{
	return (struct itimerval){
		.it_value = {.tv_sec = value->it_value.tv_sec,
			     .tv_usec = value->it_value.tv_nsec / 1000},
		.it_interval = {.tv_sec = value->it_interval.tv_sec,
				.tv_usec = value->it_interval.tv_nsec / 1000},
	};
}

static int timer_interval_admit(int which)
{
	return which == ITIMER_REAL				 ? 0
	       : which == ITIMER_VIRTUAL || which == ITIMER_PROF ? -EOPNOTSUPP
								 : -EINVAL;
}

int uk_posix_getitimer(int which, struct itimerval *output)
{
	struct posix_process *process = uk_pprocess_current();
	struct itimerspec value;
	struct itimerval result;
	int rc = timer_interval_admit(which);

	if (rc)
		return rc;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_timer_get_locked(&process->signal->timers.real, &value,
				  timer_clock(CLOCK_REALTIME),
				  timer_clock(CLOCK_MONOTONIC));
	uk_mutex_unlock(&pprocess_mutex);
	result = timer_timeval(&value);
	return uk_nofault_try_write(result, output) ? 0 : -EFAULT;
}

int uk_posix_setitimer(int which, const struct itimerval *input,
		       struct itimerval *output)
{
	struct posix_process *process = uk_pprocess_current();
	struct pprocess_retired retired = {0};
	struct itimerval value = {0}, result;
	struct itimerspec converted, old;
	int rc;

	if (input && !uk_nofault_try_read(value, input))
		return -EFAULT;
	rc = timer_interval_admit(which);
	if (rc)
		return rc;
	if (value.it_value.tv_sec < 0 || value.it_value.tv_usec < 0 ||
	    value.it_value.tv_usec >= 1000000 || value.it_interval.tv_sec < 0 ||
	    value.it_interval.tv_usec < 0 ||
	    value.it_interval.tv_usec >= 1000000)
		return -EINVAL;
	converted = (struct itimerspec){
		.it_value = {.tv_sec = value.it_value.tv_sec,
			     .tv_nsec = value.it_value.tv_usec * 1000},
		.it_interval = {.tv_sec = value.it_interval.tv_sec,
				.tv_nsec = value.it_interval.tv_usec * 1000},
	};
	uk_mutex_lock(&pprocess_mutex);
	timer_replace_locked(&process->signal->timers.real, 0, &converted,
			     output ? &old : NULL, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	if (!output)
		return 0;
	result = timer_timeval(&old);
	return uk_nofault_try_write(result, output) ? 0 : -EFAULT;
}

unsigned int uk_posix_alarm(unsigned int seconds)
{
	struct itimerval value = {.it_value.tv_sec = seconds}, old;
	unsigned int remaining;
	int rc = uk_posix_setitimer(ITIMER_REAL, &value, &old);

	UK_BUGON(rc);
	remaining = old.it_value.tv_sec;
	if (old.it_value.tv_usec >= 500000 ||
	    (!remaining && old.it_value.tv_usec))
		remaining++;
	return remaining;
}
