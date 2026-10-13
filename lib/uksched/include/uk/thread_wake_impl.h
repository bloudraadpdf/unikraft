/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_THREAD_WAKE_IMPL_H
#define UK_THREAD_WAKE_IMPL_H

#include <uk/sched.h>
#include <uk/plat/spinlock.h>

static inline void uk_thread_wake_locked(struct uk_thread *thread, bool isr)
{
	struct uk_sched *scheduler = thread->sched;

	if (uk_thread_is_exited(thread))
		return;
	if (!uk_thread_is_runnable(thread)) {
		uk_thread_set_runnable(thread);
		if (scheduler) {
			if (isr)
				scheduler->thread_woken_isr(scheduler, thread);
			else
				scheduler->thread_woken(scheduler, thread);
		}
	}
	thread->wakeup_time = 0;
}

static inline void uk_sched_wake_cpu(struct uk_sched *scheduler __maybe_unused)
{
#if CONFIG_HAVE_SMP
	if (scheduler->cpu_index != uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx)) {
		unsigned int count = 1;
		int rc = uk_lcpu_wakeup(&scheduler->cpu_index, &count);

		if (rc || count != 1)
			UK_CRASH("Remote CPU wake failed: %d\n", rc);
	}
#endif
}

static inline void uk_thread_wake_impl(struct uk_thread *thread, bool isr)
{
	struct uk_sched *scheduler = thread->sched;
	unsigned long flags = uk_lcpu_save_irqf();

	if (scheduler)
		ukarch_spin_lock(&scheduler->lock);
	uk_thread_wake_locked(thread, isr);
	if (scheduler)
		ukarch_spin_unlock(&scheduler->lock);
	if (scheduler)
		uk_sched_wake_cpu(scheduler);

	uk_lcpu_restore_irqf(flags);
}

#endif
