/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/futex.h>
#include "process.h"

struct posix_vm_context *uk_posix_futex_vm_get(struct uk_thread *thread)
{
	struct posix_thread *target;
	struct posix_vm_context *state = NULL;

	UK_ASSERT(thread && (thread->flags & UK_THREADF_UKTLS));
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	target = uk_thread_uktls_var(thread, pthread_self);
	if (target && target->process) {
		state = target->process->vm_context;
		if (state)
			state->references++;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return state;
}
