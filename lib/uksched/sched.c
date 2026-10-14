/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Costin Lupu <costin.lupu@cs.pub.ro>
 *          Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
 * Copyright (c) 2022, NEC Laboratories GmbH, NEC Corrporation,
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

#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <uk/arch/util.h>
#include <uk/plat/config.h>
#include <uk/plat/time.h>
#include <uk/alloc.h>
#include <uk/lcpu.h>
#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread_wake_impl.h>
#include <uk/syscall.h>
#include <uk/wait.h>
#include <uk/pcpuvar.h>

struct uk_sched *uk_sched_head;

__uk_pcpuvar struct uk_thread *__uk_sched_thread_current;

int uk_sched_register(struct uk_sched *s)
{
	struct uk_sched *this = uk_sched_head;

	if (!uk_sched_head) {
		uk_sched_head = s;
		s->next = NULL;
		return 0;
	}

	while (this && this->next)
		this = this->next;
	this->next = s;
	s->next = NULL;
	return 0;
}

struct uk_thread *uk_sched_thread_create_fn0(struct uk_sched *s,
					     uk_thread_fn0_t fn0,
					     size_t stack_len,
					     size_t auxstack_len,
					     bool no_uktls,
					     bool no_ectx,
					     const char *name,
					     void *priv,
					     uk_thread_dtor_t dtor)
{
	struct uk_thread *t;
	int rc;

	UK_ASSERT(s);
	UK_ASSERT(s->a_stack);
	UK_ASSERT(s->a_auxstack);

	if (!no_uktls && !s->a_uktls)
		goto err_out;

	t = uk_thread_create_fn0(s->a,
				 fn0,
				 s->a_stack, stack_len,
				 s->a_auxstack, auxstack_len,
				 no_uktls ? NULL : s->a_uktls,
				 no_ectx,
				 name,
				 priv,
				 dtor);
	if (!t)
		goto err_out;

	rc = uk_sched_thread_add(s, t);
	if (rc < 0)
		goto err_free_t;

	return t;

err_free_t:
	uk_thread_release(t);
err_out:
	return NULL;
}

struct uk_thread *uk_sched_thread_create_fn1(struct uk_sched *s,
					     uk_thread_fn1_t fn1,
					     void *argp,
					     size_t stack_len,
					     size_t auxstack_len,
					     bool no_uktls,
					     bool no_ectx,
					     const char *name,
					     void *priv,
					     uk_thread_dtor_t dtor)
{
	struct uk_thread *t;
	int rc;

	UK_ASSERT(s);
	UK_ASSERT(s->a_stack);
	UK_ASSERT(s->a_auxstack);

	if (!no_uktls && !s->a_uktls)
		goto err_out;

	t = uk_thread_create_fn1(s->a,
				 fn1, argp,
				 s->a_stack, stack_len,
				 s->a_auxstack, auxstack_len,
				 no_uktls ? NULL : s->a_uktls,
				 no_ectx,
				 name,
				 priv,
				 dtor);
	if (!t)
		goto err_out;

	rc = uk_sched_thread_add(s, t);
	if (rc < 0)
		goto err_free_t;

	return t;

err_free_t:
	uk_thread_release(t);
err_out:
	return NULL;
}

struct uk_thread *uk_sched_thread_create_fn2(struct uk_sched *s,
					     uk_thread_fn2_t fn2,
					     void *argp0, void *argp1,
					     size_t stack_len,
					     size_t auxstack_len,
					     bool no_uktls,
					     bool no_ectx,
					     const char *name,
					     void *priv,
					     uk_thread_dtor_t dtor)
{
	struct uk_thread *t;
	int rc;

	UK_ASSERT(s);
	UK_ASSERT(s->a_stack);
	UK_ASSERT(s->a_auxstack);

	if (!no_uktls && !s->a_uktls)
		goto err_out;

	t = uk_thread_create_fn2(s->a,
				 fn2, argp0, argp1,
				 s->a_stack, stack_len,
				 s->a_auxstack, auxstack_len,
				 no_uktls ? NULL : s->a_uktls,
				 no_ectx,
				 name,
				 priv,
				 dtor);
	if (!t)
		goto err_out;

	rc = uk_sched_thread_add(s, t);
	if (rc < 0)
		goto err_free_t;

	return t;

err_free_t:
	uk_thread_release(t);
err_out:
	return NULL;
}

static bool sched_has_garbage(struct uk_sched *sched)
{
	unsigned long flags;
	bool ready;

	ukplat_spin_lock_irqsave(&sched->lock, flags);
	ready = !UK_TAILQ_EMPTY(&sched->exited_threads);
	ukplat_spin_unlock_irqrestore(&sched->lock, flags);
	return ready;
}

static __noreturn void sched_reap(void *arg)
{
	struct uk_sched *sched = arg;

	for (;;) {
		uk_waitq_wait_event(&sched->gc_wait, sched_has_garbage(sched));
		uk_sched_thread_gc(sched);
	}
}

int uk_sched_start(struct uk_sched *s)
{
	struct uk_thread *main_thread;
	uintptr_t tlsp;
	uintptr_t auxsp;
	int ret;

	UK_ASSERT(s);
	UK_ASSERT(s->sched_start);
	UK_ASSERT(!s->is_started);
	s->cpu_index = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	UK_ASSERT(!uk_thread_current()); /* No other thread runs */

	/* Allocate an `uk_thread` instance for current context
	 * NOTE: We assume that if we have a TLS pointer, it points to
	 *       an TLS that is derived from the Unikraft TLS template.
	 */
	tlsp = uk_lcpu_tlsp_get();
	auxsp = uk_pcpuvar_current_get(UK_LCPU_AUXSP_SYM);
	main_thread = uk_thread_create_bare(s->a,
					    0x0, 0x0, auxsp,
					    tlsp, !(!tlsp), false,
					    "init", NULL, NULL);
	if (!main_thread) {
		ret = -ENOMEM;
		goto err_out;
	}
	main_thread->sched = s;
	s->current = main_thread;

	/* Because `main_thread` acts as container for storing the current
	 * context, it does not have IP and SP set. We have to manually mark
	 * the thread as RUNNABLE.
	 */
	uk_thread_set_runnable(main_thread);

	/* Set main_thread as current scheduled thread */
	uk_pcpuvar_current_set(__uk_sched_thread_current, main_thread);

	/* Add main to the scheduler's thread list */
	UK_TAILQ_INSERT_TAIL(&s->thread_list, main_thread, thread_list);

	/* Enable scheduler, like time slicing, etc. and notify that `s`
	 * has an (already) scheduled thread
	 */
	ret = s->sched_start(s, main_thread);
	if (ret < 0)
		goto err_unset_thread_current;
	s->is_started = true;
	s->reaper = uk_sched_thread_create_fn1(s, sched_reap, s,
		0, 0, false, false, "reaper", NULL, NULL);
	if (!s->reaper) {
		s->is_started = false;
		ret = -ENOMEM;
		goto err_unset_thread_current;
	}

#if CONFIG_LIBUKSCHED_STATS
	s->start_time = (__u64)ukplat_monotonic_clock();
#endif /* CONFIG_LIBUKSCHED_STATS */

	return 0;

err_unset_thread_current:
	UK_TAILQ_REMOVE(&s->thread_list, main_thread, thread_list);
	main_thread->sched = NULL;
	s->current = NULL;
	uk_pcpuvar_current_set(__uk_sched_thread_current, NULL);
	uk_thread_release(main_thread);
err_out:
	return ret;
}

unsigned int uk_sched_thread_gc(struct uk_sched *sched)
{
	struct uk_thread *thread;
	unsigned int num = 0;
	unsigned long flags;

	UK_ASSERT(sched->cpu_index == uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx));
	UK_ASSERT(uk_thread_current() == sched->reaper);
	UK_ASSERT(!uk_lcpu_irqs_disabled());
	for (;;) {
		ukplat_spin_lock_irqsave(&sched->lock, flags);
		thread = UK_TAILQ_FIRST(&sched->exited_threads);
		if (!thread) {
			ukplat_spin_unlock_irqrestore(&sched->lock, flags);
			break;
		}
		UK_TAILQ_REMOVE(&sched->exited_threads, thread, thread_list);
		ukplat_spin_unlock_irqrestore(&sched->lock, flags);
		UK_ASSERT(thread != uk_thread_current());
		UK_ASSERT(uk_thread_is_exited(thread));

		uk_pr_debug("%p: garbage collect thread %p (%s)\n",
			    sched, thread,
			    thread->name ? thread->name : "<unnamed>");

		if (thread->_gc_fn)
			thread->_gc_fn(thread,  thread->_gc_argp);
		uk_thread_release(thread);
		++num;
	}

	return num;
}

static void sched_stop_complete(struct uk_thread *thread)
{
	struct uk_waitq_ticket *ticket;
	unsigned long flags;

	if (uk_thread_in_waitq(thread))
		uk_waitq_cancel(thread);
	thread->wakeup_time = 0;
	_uk_waitq_lock(&thread->stop_wait, flags);
	__atomic_store_n(&thread->stop_complete, true, __ATOMIC_RELEASE);
	uk_list_for_each_entry(ticket, &thread->stop_wait.waiters, link)
		_uk_waitq_wake(&thread->stop_wait, ticket);
	_uk_waitq_unlock(&thread->stop_wait, flags);
}

unsigned int uk_sched_process_stop_requests(struct uk_sched *sched)
{
	struct uk_thread *thread;
	unsigned long flags;
	unsigned int count = 0;

	UK_ASSERT(sched->cpu_index == uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx));
	UK_ASSERT(uk_thread_current() == sched->idle_thread(sched, 0));
	for (;;) {
		ukplat_spin_lock_irqsave(&sched->lock, flags);
		thread = UK_TAILQ_FIRST(&sched->stop_requests);
		if (thread)
			UK_TAILQ_REMOVE(&sched->stop_requests, thread, queue);
		ukplat_spin_unlock_irqrestore(&sched->lock, flags);
		if (!thread)
			return count;
		sched_stop_complete(thread);
		++count;
	}
}

void uk_sched_thread_stop(struct uk_thread *thread)
{
	struct uk_sched *sched;
	unsigned long flags;
	bool local, requested = false;

	UK_ASSERT(thread);
	UK_ASSERT(thread != uk_thread_current());
	sched = thread->sched;
	if (!sched)
		return;
	UK_ASSERT(thread != sched->idle_thread(sched, 0));
	local = !sched->is_started ||
		sched->cpu_index == uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	UK_ASSERT(!uk_lcpu_irqs_disabled());

	ukplat_spin_lock_irqsave(&sched->lock, flags);
	if (!uk_thread_is_stopping(thread)) {
		sched->thread_remove(sched, thread);
		thread->flags |= UK_THREADF_STOPPING;
		uk_thread_set_blocked(thread);
		if (!local)
			UK_TAILQ_INSERT_TAIL(&sched->stop_requests, thread, queue);
		requested = true;
	}
	ukplat_spin_unlock_irqrestore(&sched->lock, flags);
	if (requested) {
		if (local)
			sched_stop_complete(thread);
		else
			uk_sched_wake_cpu(sched);
	}
	uk_waitq_wait_event(&thread->stop_wait,
		__atomic_load_n(&thread->stop_complete, __ATOMIC_ACQUIRE));
	/* The owner must finish its final access before the caller can free it. */
	_uk_waitq_lock(&thread->stop_wait, flags);
	_uk_waitq_unlock(&thread->stop_wait, flags);
}

void uk_sched_thread_terminate(struct uk_thread *thread)
{
	struct uk_sched *sched;
	unsigned long flags;

	UK_ASSERT(thread);
	 /* NOTE: The following assertion can also fail on a double-termination.
	  *       This can happen when the thread was already put on the
	  *       exited_thread list and waits for getting garbage collected.
	  *       Double-termination can be avoided by testing for the exited
	  *       flag.
	  */
	UK_ASSERT(thread->sched);

	sched = thread->sched;

	uk_pr_debug("%p: thread %p (%s) terminated\n",
		    sched, thread, thread->name ? thread->name : "<unnamed>");

	if (thread == uk_thread_current() && uk_thread_in_waitq(thread))
		uk_waitq_cancel(thread);
	/* remove from scheduling queue */
	uk_sched_thread_remove(thread);
	/* causes calling termination table */
	uk_thread_set_exited(thread);

	if (thread == uk_thread_current()) {
		/* enqueue thread for garbage collecting */
		uk_pr_debug("%p: thread %p (%s) on gc list\n",
			    sched, thread, thread->name ?
					   thread->name : "<unnamed>");
		ukplat_spin_lock_irqsave(&sched->lock, flags);
		UK_TAILQ_INSERT_TAIL(&sched->exited_threads, thread,
				     thread_list);
		ukplat_spin_unlock_irqrestore(&sched->lock, flags);

		uk_waitq_wake_up(&sched->gc_wait);

		/* leave this thread */
		sched->yield(sched); /* we won't return */
		UK_CRASH("Unexpectedly returned to exited thread %p\n", thread);
	} else {
		/* free thread resources immediately */
		uk_thread_release(thread);
	}
}

/* This function has the __noreturn attribute set */
void uk_sched_thread_exit2(uk_thread_gc_t gc_fn, void *gc_argp)
{
	struct uk_thread *t = uk_thread_current();

	t->_gc_fn   = gc_fn;
	t->_gc_argp = gc_argp;
	uk_sched_thread_terminate(t);
	UK_CRASH("Unexpectedly returned to exited thread %p\n", t);
}

/* This function has the __noreturn attribute set */
void uk_sched_thread_exit(void)
{
	uk_sched_thread_exit2(NULL, NULL);
}

void uk_sched_thread_sleep(__nsec nsec)
{
	struct uk_thread *thread;

	thread = uk_thread_current();
	uk_thread_block_timeout(thread, nsec);
	uk_sched_yield();
}

int uk_sched_thread_add(struct uk_sched *s, struct uk_thread *t)
{
	unsigned long flags;
	int rc;

	UK_ASSERT(s);
	UK_ASSERT(t);
	UK_ASSERT(!t->sched);

	ukplat_spin_lock_irqsave(&s->lock, flags);

	rc = s->thread_add(s, t);
	if (rc < 0)
		goto out;

	t->sched = s;
	UK_TAILQ_INSERT_TAIL(&s->thread_list, t, thread_list);
out:
	ukplat_spin_unlock_irqrestore(&s->lock, flags);
	if (!rc && s->is_started)
		uk_sched_wake_cpu(s);
	return rc;
}

int uk_sched_thread_remove(struct uk_thread *t)
{
	unsigned long flags;
	struct uk_sched *s;

	UK_ASSERT(t);
	UK_ASSERT(t->sched);

	s = t->sched;
	if (t != uk_thread_current())
		uk_sched_thread_stop(t);
	ukplat_spin_lock_irqsave(&s->lock, flags);
	s->thread_remove(s, t);
	t->sched = NULL;
	UK_TAILQ_REMOVE(&s->thread_list, t, thread_list);
	ukplat_spin_unlock_irqrestore(&s->lock, flags);
	return 0;
}

UK_SYSCALL_R_DEFINE(int, sched_yield)
{
	uk_sched_yield();
	return 0;
}

void uk_sched_dumpk_threads(int klvl, struct uk_sched *s)
{
	struct uk_thread *t, *tmp;

	if (uk_sched_current() == s) {
		/* Let the scheduler update the thread execution time
		 * of the current thread with a forced context switch.
		 */
		uk_sched_yield();
	}

	uk_printk(klvl, "sched %p:\n", s);
	uk_sched_foreach_thread_safe(s, t, tmp) {
		uk_printk(klvl,
			  " + thread %p (%s), ctx: %p, "
			  "execution time: %"__PRInsec".%06"__PRInsec"s, "
			  "flags: %c%c%c%c%c\n",
			  t, t->name ? t->name : "<unnamed>",
			  &t->ctx,
			  ukarch_time_nsec_to_sec(t->exec_time),
			  ukarch_time_nsec_to_usec(t->exec_time) % 1000000,
			  (t->flags & UK_THREADF_RUNNABLE) ? 'R' : '-',
			  (t->flags & UK_THREADF_EXITED)   ? 'D' : '-',
			  (t->flags & UK_THREADF_ECTX)     ? 'E' : '-',
			  (t->flags & UK_THREADF_AUXSP)    ? 'A' : '-',
			  (t->flags & UK_THREADF_UKTLS)    ? 'T' : '-');
	}
}

int uk_sched_get_cpu_mask(size_t size, unsigned long *mask)
{
	const size_t word_bits = sizeof(*mask) * CHAR_BIT;
	size_t bytes = ALIGN_UP(uk_lcpu_count(), word_bits) / CHAR_BIT;
	struct uk_sched *scheduler;

	if (size < bytes || size % sizeof(*mask))
		return -EINVAL;
	if (!mask)
		return -EFAULT;
	memset(mask, 0, bytes);
	for (scheduler = uk_sched_head; scheduler; scheduler = scheduler->next)
		if (scheduler->is_started)
			mask[scheduler->cpu_index / word_bits] |=
				1UL << (scheduler->cpu_index % word_bits);
	return bytes;
}

UK_SYSCALL_R_DEFINE(int, sched_setaffinity, int, pid, long, cpusetsize,
						unsigned long*, mask)
{
	UK_WARN_STUBBED();
	return 0;
}

static void sched_membarrier_local(bool sync_core)
{
	uk_arch_mb();
	if (!sync_core)
		return;
#if CONFIG_ARCH_ARM_64
	uk_arch_arm64_isb();
#elif CONFIG_ARCH_X86_64
	__u32 eax, ebx, ecx, edx;

	uk_arch_x86_64_cpuid(0, 0, &eax, &ebx, &ecx, &edx);
#endif
	uk_arch_mb();
}

#if CONFIG_HAVE_SMP
struct sched_membarrier_call {
	bool sync_core;
	unsigned int completed;
};

static void sched_membarrier_remote(struct uk_lcpu_regs *regs __unused,
				   void *arg)
{
	struct sched_membarrier_call *call = arg;

	sched_membarrier_local(call->sync_core);
	__atomic_store_n(&call->completed, 1, __ATOMIC_RELEASE);
}
#endif

int uk_sched_membarrier(bool sync_core)
{
	uk_arch_mb();
#if CONFIG_HAVE_SMP
	struct sched_membarrier_call call = { .sync_core = sync_core };
	struct uk_lcpu_func fn = {
		.fn = sched_membarrier_remote, .user = &call
	};
	__u64 current = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	struct uk_sched *s;
	unsigned int count;
	int rc;

	UK_ASSERT(!uk_lcpu_irqs_disabled());
	for (s = uk_sched_head; s; s = s->next) {
		__u64 target = s->cpu_index;

		if (!s->is_started || target == current)
			continue;
		call.completed = 0;
		count = 1;
		rc = uk_lcpu_run(&target, &count, &fn, 0);
		if (rc)
			return rc;
		while (!__atomic_load_n(&call.completed, __ATOMIC_ACQUIRE))
			uk_arch_spinwait();
	}
#endif
	sched_membarrier_local(sync_core);
	return 0;
}

struct uk_sched *uk_sched_select(void)
{
	static unsigned int next = 1;
	struct uk_sched *scheduler;
	unsigned int count = 0;
	unsigned int selected;

	for (scheduler = uk_sched_head; scheduler; scheduler = scheduler->next)
		if (scheduler->is_started)
			count++;
	UK_ASSERT(count);
	selected = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % count;
	for (scheduler = uk_sched_head; scheduler; scheduler = scheduler->next)
		if (scheduler->is_started && !selected--)
			return scheduler;
	UK_CRASH("No started scheduler selected");
}
