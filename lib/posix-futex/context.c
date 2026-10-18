/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/alloc.h>
#include <uk/futex.h>
#include <uk/refcount.h>
#include <uk/thread.h>

static __uk_tls struct uk_futex_call *active_calls;

static int context_thread_init(struct uk_thread *child,
			       struct uk_thread *parent __unused)
{
	uk_thread_uktls_var(child, active_calls) = NULL;
	return 0;
}

UK_THREAD_INIT_PRIO_FLAGS(context_thread_init, 0, UK_PRIO_EARLIEST,
			  UK_THREAD_INITF_UKTLS);

void uk_futex_call_put(struct uk_futex_call *call,
		       struct uk_futex_call **retired)
{
	if (!call || !uk_refcount_release(&call->references))
		return;
	call->retired_next = *retired;
	*retired = call;
}

void uk_futex_call_get(struct uk_futex_call *call)
{
	uk_refcount_acquire(&call->references);
}

void uk_futex_retired_release(struct uk_futex_call *retired)
{
	struct uk_futex_call *next;

	while (retired) {
		next = retired->retired_next;
		uk_futex_key_put(&retired->keys[0]);
		uk_futex_key_put(&retired->keys[1]);
		uk_free(retired->allocator, retired);
		retired = next;
	}
}

struct uk_futex_call *uk_futex_call_create(struct uk_thread *thread,
					   struct uk_alloc *allocator)
{
	struct uk_futex_call *call;

	UK_ASSERT(thread && (thread->flags & UK_THREADF_UKTLS));
	if (!allocator)
		return NULL;
	call = uk_zalloc(allocator, sizeof(*call));
	if (!call)
		return NULL;
	call->allocator = allocator;
	call->waiter.thread = thread;
	UK_INIT_LIST_HEAD(&call->waiter.link);
	UK_INIT_LIST_HEAD(&call->waiter.pi_link);
	uk_refcount_init(&call->references, 1);
	call->active_next = uk_thread_uktls_var(thread, active_calls);
	uk_thread_uktls_var(thread, active_calls) = call;
	return call;
}

void uk_futex_waiter_key_set(struct uk_futex_waiter *waiter,
			     struct uk_futex_call *call, unsigned int index,
			     struct uk_futex_call **retired)
{
	struct uk_futex_call *old = waiter->key_owner;

	UK_ASSERT(index < 2);
	uk_refcount_acquire(&call->references);
	waiter->key_owner = call;
	waiter->key = &call->keys[index];
	uk_futex_call_put(old, retired);
}

void uk_futex_call_end(struct uk_thread *thread, struct uk_futex_call *call)
{
	struct uk_futex_call **link =
		&uk_thread_uktls_var(thread, active_calls);
	struct uk_futex_call *retired = NULL;

	UK_ASSERT(uk_list_empty(&call->waiter.link));
	UK_ASSERT(uk_list_empty(&call->waiter.pi_link));
	while (*link && *link != call)
		link = &(*link)->active_next;
	UK_ASSERT(*link == call);
	*link = call->active_next;
	call->active_next = NULL;
	uk_futex_call_put(call->waiter.key_owner, &retired);
	call->waiter.key_owner = NULL;
	call->waiter.key = NULL;
	uk_futex_call_put(call, &retired);
	uk_futex_retired_release(retired);
}

void uk_futex_call_abort_all(struct uk_thread *thread)
{
	struct uk_futex_call *call;

	while ((call = uk_thread_uktls_var(thread, active_calls)))
		uk_futex_call_end(thread, call);
}
