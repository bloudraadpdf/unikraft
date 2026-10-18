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
#include <uk/errptr.h>
#include <uk/lcpu.h>
#include <uk/sched.h>
#include <uk/sched_impl.h>
#include <uk/thread_wake_impl.h>
#include <uk/syscall.h>
#include <uk/wait.h>
#include <uk/pcpuvar.h>

struct uk_sched *uk_sched_head;

__uk_pcpuvar struct uk_thread *__uk_sched_thread_current;

__nsec uk_sched_current_exec_time(void)
{
	struct uk_sched *scheduler = uk_sched_current();
	unsigned long flags;
	__nsec time;

	UK_ASSERT(scheduler && scheduler->current_exec_time);
	ukplat_spin_lock_irqsave(&scheduler->lock, flags);
	time = scheduler->current_exec_time(scheduler);
	ukplat_spin_unlock_irqrestore(&scheduler->lock, flags);
	return time;
}

struct uk_sched_affinity {
	UK_TAILQ_ENTRY(struct uk_sched_affinity) link;
	struct uk_thread *thread;
	struct uk_alloc *allocator;
	unsigned int references;
	struct uk_waitq wait;
	unsigned long mask[UK_THREAD_CPU_MASK_WORDS];
	bool complete;
	bool submitted;
	int result;
};

static __spinlock affinity_transfer_lock = UKARCH_SPINLOCK_INITIALIZER();

static bool sched_cpu_allowed(const unsigned long *mask,
			      const struct uk_sched *s)
{
	const size_t bits = sizeof(*mask) * CHAR_BIT;

	UK_ASSERT(s->cpu_index < CONFIG_UKPLAT_CPU_MAXCOUNT);
	return (mask[s->cpu_index / bits] & (1UL << (s->cpu_index % bits))) !=
	       0;
}

static void affinity_cancel_locked(struct uk_thread *thread,
				   struct uk_sched_affinity_list *cancelled);
static void affinity_complete_list(struct uk_sched_affinity_list *cancelled);
static void affinity_abandon(struct uk_thread *thread);

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
	affinity_abandon(thread);
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
	struct uk_sched_affinity_list cancelled;

	UK_ASSERT(thread);
	UK_ASSERT(thread != uk_thread_current());
	UK_ASSERT(!uk_lcpu_irqs_disabled());

	sched = uk_thread_sched_lock(thread, &flags);
	if (!sched) {
		uk_thread_sched_unlock(sched, flags);
		return;
	}
	UK_ASSERT(thread != sched->idle_thread(sched, 0));
	local = !sched->is_started ||
		sched->cpu_index == uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	UK_TAILQ_INIT(&cancelled);
	affinity_cancel_locked(thread, &cancelled);
	if (!uk_thread_is_stopping(thread)) {
		sched->thread_remove(sched, thread);
		thread->flags |= UK_THREADF_STOPPING;
		uk_thread_set_blocked(thread);
		if (!local)
			UK_TAILQ_INSERT_TAIL(&sched->stop_requests, thread, queue);
		requested = true;
	}
	uk_thread_sched_unlock(sched, flags);
	affinity_complete_list(&cancelled);
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

	if (thread == uk_thread_current()) {
		if (uk_thread_in_waitq(thread))
			uk_waitq_cancel(thread);
		affinity_abandon(thread);
		uk_thread_set_exited(thread);
	}
	/* remove from scheduling queue */
	sched = thread->sched;
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
	if (!sched_cpu_allowed(t->allowed_cpus, s))
		return -EINVAL;

	ukplat_spin_lock_irqsave(&s->lock, flags);

	rc = s->thread_add(s, t);
	if (rc < 0)
		goto out;

	__atomic_store_n(&t->sched, s, __ATOMIC_RELEASE);
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
	struct uk_sched_affinity_list cancelled;

	UK_ASSERT(t);
	if (t != uk_thread_current())
		uk_sched_thread_stop(t);
	s = uk_thread_sched_lock(t, &flags);
	UK_ASSERT(s);
	UK_TAILQ_INIT(&cancelled);
	affinity_cancel_locked(t, &cancelled);
	s->thread_remove(s, t);
	__atomic_store_n(&t->sched, NULL, __ATOMIC_RELEASE);
	UK_TAILQ_REMOVE(&s->thread_list, t, thread_list);
	uk_thread_sched_unlock(s, flags);
	affinity_complete_list(&cancelled);
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

struct uk_sched *uk_sched_select(const struct uk_thread *thread)
{
	static unsigned int next = 1;
	struct uk_sched *scheduler;
	unsigned int count = 0;
	unsigned int selected;

	for (scheduler = uk_sched_head; scheduler; scheduler = scheduler->next)
		if (scheduler->is_started &&
		    sched_cpu_allowed(thread->allowed_cpus, scheduler))
			count++;
	UK_ASSERT(count);
	selected = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED) % count;
	for (scheduler = uk_sched_head; scheduler; scheduler = scheduler->next)
		if (scheduler->is_started &&
		    sched_cpu_allowed(thread->allowed_cpus, scheduler) &&
		    !selected--)
			return scheduler;
	UK_CRASH("No started scheduler selected");
}

bool uk_sched_has_higher_priority(void)
{
	struct uk_thread *current = uk_thread_current(), *thread;
	struct uk_sched *scheduler = current->sched;
	unsigned long flags;
	bool found = false;

	ukplat_spin_lock_irqsave(&scheduler->lock, flags);
	uk_sched_foreach_thread(scheduler, thread) {
		if (uk_thread_is_runnable(thread) && !uk_thread_is_exited(thread) &&
		    uk_load_n(&thread->effective_priority) >
		    uk_load_n(&current->effective_priority)) {
			found = true;
			break;
		}
	}
	ukplat_spin_unlock_irqrestore(&scheduler->lock, flags);
	return found;
}

static void affinity_release(struct uk_sched_affinity *request)
{
	if (!__atomic_sub_fetch(&request->references, 1, __ATOMIC_ACQ_REL))
		uk_free(request->allocator, request);
}

static void affinity_complete(struct uk_sched_affinity *request, int result)
{
	struct uk_waitq_ticket *ticket;
	unsigned long flags;

	_uk_waitq_lock(&request->wait, flags);
	request->result = result;
	__atomic_store_n(&request->complete, true, __ATOMIC_RELEASE);
	uk_list_for_each_entry(ticket, &request->wait.waiters, link)
		_uk_waitq_wake(&request->wait, ticket);
	_uk_waitq_unlock(&request->wait, flags);
	affinity_release(request);
}

static void affinity_complete_list(struct uk_sched_affinity_list *cancelled)
{
	struct uk_sched_affinity *request;

	while ((request = UK_TAILQ_FIRST(cancelled))) {
		UK_TAILQ_REMOVE(cancelled, request, link);
		affinity_complete(request, -ESRCH);
	}
}

static unsigned int
affinity_move_requests(struct uk_thread *thread,
		       struct uk_sched_affinity_list *source,
		       struct uk_sched_affinity_list *destination)
{
	struct uk_sched_affinity *request, *next;
	unsigned int count = 0;

	UK_TAILQ_FOREACH_SAFE(request, source, link, next) {
		if (request->thread != thread)
			continue;
		UK_TAILQ_REMOVE(source, request, link);
		UK_TAILQ_INSERT_TAIL(destination, request, link);
		count++;
	}
	return count;
}

static void affinity_cancel_locked(struct uk_thread *thread,
				   struct uk_sched_affinity_list *cancelled)
{
	unsigned int count =
		affinity_move_requests(thread,
				       &thread->sched->affinity_requests,
				       cancelled);

	__atomic_sub_fetch(&thread->affinity_pending, count, __ATOMIC_RELEASE);
	UK_ASSERT(!thread->affinity_pending);
}

struct uk_sched_affinity *uk_sched_affinity_create(struct uk_alloc *allocator,
						   const unsigned long *mask)
{
	struct uk_thread *caller = uk_thread_current();
	struct uk_sched_affinity *request;
	struct uk_sched *owner;
	unsigned long flags;

	request = uk_zalloc(allocator, sizeof(*request));
	if (!request)
		return ERR2PTR(-ENOMEM);
	request->allocator = allocator;
	request->references = 1;
	memcpy(request->mask, mask, sizeof(request->mask));
	uk_waitq_init(&request->wait);
	owner = uk_thread_sched_lock(caller, &flags);
	UK_ASSERT(!caller->affinity_wait);
	caller->affinity_wait = request;
	uk_thread_sched_unlock(owner, flags);
	return request;
}

static void affinity_abandon(struct uk_thread *thread)
{
	struct uk_sched_affinity *request;
	struct uk_sched *owner;
	unsigned long flags;

	owner = uk_thread_sched_lock(thread, &flags);
	request = thread->affinity_wait;
	thread->affinity_wait = NULL;
	uk_thread_sched_unlock(owner, flags);
	if (request)
		affinity_release(request);
}

void uk_sched_affinity_discard(struct uk_sched_affinity *request)
{
	UK_ASSERT(uk_thread_current()->affinity_wait == request);
	affinity_abandon(uk_thread_current());
}

int uk_sched_thread_get_affinity(struct uk_thread *thread, size_t size,
				 unsigned long *mask)
{
	struct uk_sched *owner;
	unsigned long flags;
	int rc = uk_sched_get_cpu_mask(size, mask);

	if (rc < 0)
		return rc;
	owner = uk_thread_sched_lock(thread, &flags);
	if (uk_thread_is_exited(thread) || uk_thread_is_stopping(thread))
		rc = -ESRCH;
	else
		for (size_t i = 0; i < (size_t)rc / sizeof(*mask); i++)
			mask[i] &= thread->allowed_cpus[i];
	uk_thread_sched_unlock(owner, flags);
	return rc;
}

void uk_sched_thread_inherit_affinity(struct uk_thread *child,
				      struct uk_thread *parent)
{
	struct uk_sched *owner;
	unsigned long flags;

	UK_ASSERT(!child->sched);
	owner = uk_thread_sched_lock(parent, &flags);
	memcpy(child->allowed_cpus, parent->allowed_cpus,
	       sizeof(child->allowed_cpus));
	uk_thread_sched_unlock(owner, flags);
}

void uk_sched_thread_notify(struct uk_thread *thread)
{
	struct uk_sched *owner;
	unsigned long flags;

	owner = uk_thread_sched_lock(thread, &flags);
	if (owner)
		uk_sched_wake_cpu(owner);
	uk_thread_sched_unlock(owner, flags);
}

int uk_sched_thread_affinity_submit(struct uk_thread *thread,
				    struct uk_sched_affinity *request)
{
	unsigned long online[UK_THREAD_CPU_MASK_WORDS] = {0};
	struct uk_sched *owner;
	unsigned long flags;
	bool any = false, move = false;
	int rc;

	UK_ASSERT(uk_thread_current()->affinity_wait == request);
	if (__atomic_exchange_n(&request->submitted, true, __ATOMIC_ACQ_REL))
		return -EALREADY;
	request->thread = thread;
	rc = uk_sched_get_cpu_mask(sizeof(online), online);
	if (rc < 0)
		return rc;
	for (size_t i = 0; i < UK_THREAD_CPU_MASK_WORDS; i++) {
		request->mask[i] &= online[i];
		any |= request->mask[i] != 0;
	}
	if (!any)
		return -EINVAL;
	owner = uk_thread_sched_lock(thread, &flags);
	if (!owner || uk_thread_is_exited(thread) ||
	    uk_thread_is_stopping(thread)) {
		rc = -ESRCH;
	} else {
		__atomic_add_fetch(&request->references, 1, __ATOMIC_RELAXED);
		if (sched_cpu_allowed(request->mask, owner)) {
			memcpy(thread->allowed_cpus, request->mask,
			       sizeof(request->mask));
		} else {
			UK_TAILQ_INSERT_TAIL(&owner->affinity_requests, request,
					     link);
			__atomic_add_fetch(&thread->affinity_pending, 1,
					   __ATOMIC_RELEASE);
			move = true;
		}
		rc = 0;
	}
	uk_thread_sched_unlock(owner, flags);
	if (move)
		uk_sched_wake_cpu(owner);
	else if (!rc)
		affinity_complete(request, 0);
	return rc;
}

int uk_sched_thread_affinity_wait(struct uk_sched_affinity *request)
{
	int result;

	uk_waitq_wait_event(&request->wait, __atomic_load_n(&request->complete,
							    __ATOMIC_ACQUIRE));
	result = request->result;
	uk_sched_affinity_discard(request);
	return result;
}

static int affinity_move(struct uk_sched *owner, struct uk_sched *destination,
			 struct uk_thread *thread, const unsigned long *mask)
{
	__snsec deadline;
	int result;

	ukarch_spin_lock(&destination->lock);
	deadline = thread->wakeup_time;
	owner->thread_remove(owner, thread);
	UK_TAILQ_REMOVE(&owner->thread_list, thread, thread_list);
	thread->wakeup_time = deadline;
	result = destination->thread_add(destination, thread);
	if (!result) {
		UK_TAILQ_INSERT_TAIL(&destination->thread_list, thread,
				     thread_list);
		memcpy(thread->allowed_cpus, mask,
		       sizeof(thread->allowed_cpus));
		affinity_move_requests(thread, &owner->affinity_requests,
				       &destination->affinity_requests);
		__atomic_store_n(&thread->sched, destination, __ATOMIC_RELEASE);
	} else {
		int restored = owner->thread_add(owner, thread);

		UK_BUGON(restored);
		UK_TAILQ_INSERT_TAIL(&owner->thread_list, thread, thread_list);
	}
	ukarch_spin_unlock(&destination->lock);
	return result;
}

unsigned int uk_sched_process_affinity_requests(struct uk_sched *owner)
{
	struct uk_sched_affinity *request;
	struct uk_sched *destination;
	struct uk_thread *thread;
	unsigned long flags;
	unsigned int processed = 0;
	int result;

	UK_ASSERT(uk_thread_current() == owner->idle_thread(owner, 0));
	for (;;) {
		ukplat_spin_lock_irqsave(&affinity_transfer_lock, flags);
		ukarch_spin_lock(&owner->lock);
		request = UK_TAILQ_FIRST(&owner->affinity_requests);
		if (!request) {
			ukarch_spin_unlock(&owner->lock);
			ukplat_spin_unlock_irqrestore(&affinity_transfer_lock,
						      flags);
			return processed;
		}
		thread = request->thread;
		UK_ASSERT(thread->sched == owner && thread->affinity_pending);
		UK_TAILQ_REMOVE(&owner->affinity_requests, request, link);
		__atomic_sub_fetch(&thread->affinity_pending, 1,
				   __ATOMIC_RELEASE);
		destination = NULL;
		result = -ESRCH;
		if (!uk_thread_is_exited(thread) &&
		    !uk_thread_is_stopping(thread)) {
			if (sched_cpu_allowed(request->mask, owner)) {
				memcpy(thread->allowed_cpus, request->mask,
				       sizeof(request->mask));
				result = 0;
			} else {
				for (destination = uk_sched_head; destination;
				     destination = destination->next)
					if (destination->is_started &&
					    sched_cpu_allowed(request->mask,
							      destination))
						break;
				UK_ASSERT(destination && destination != owner);
				result = affinity_move(owner, destination,
						       thread, request->mask);
			}
		}
		ukarch_spin_unlock(&owner->lock);
		ukplat_spin_unlock_irqrestore(&affinity_transfer_lock, flags);
		if (!result && destination)
			uk_sched_wake_cpu(destination);
		affinity_complete(request, result);
		processed++;
	}
}
