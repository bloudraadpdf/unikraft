/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Copyright (c) 2022, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>

#include <uk/config.h>
#include <uk/lcpu.h>
#include <uk/process.h>
#include <uk/print.h>

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL

#include "signal.h"

int pprocess_signal_inherit_locked(struct posix_thread *child,
				   struct posix_thread *parent, __u64 flags,
				   struct uk_sigaction **previous)
{
	struct posix_process *process = child->process;

	*previous = NULL;
	if ((flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) ==
	    (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND))
		return -EINVAL;
	if ((flags & CLONE_SIGHAND) && !(flags & CLONE_VM))
		return -EINVAL;
	if ((flags & CLONE_THREAD) && !(flags & CLONE_SIGHAND))
		return -EINVAL;
	UK_ASSERT(child->signal && process && process->signal);
	UK_ASSERT(process->signal->sigaction);
	if (parent) {
		UK_ASSERT(parent->signal);
		child->signal->mask = parent->signal->mask;
		if (!(flags & CLONE_VM) || (flags & CLONE_VFORK))
			child->signal->altstack = parent->signal->altstack;
	}
	if (!(flags & CLONE_THREAD) && parent &&
	    !(flags & CLONE_CLEAR_SIGHAND)) {
		if (flags & CLONE_SIGHAND) {
			*previous = process->signal->sigaction;
			process->signal->sigaction =
				parent->process->signal->sigaction;
			pprocess_signal_sigaction_acquire(
				process->signal->sigaction);
		} else {
			memcpy(process->signal->sigaction->ks,
			       parent->process->signal->sigaction->ks,
			       sizeof(process->signal->sigaction->ks));
		}
	}
	return 0;
}

static int uk_posix_clone_sighand(void *arg)
{
	struct posix_process_clone_event_data *event = arg;
	struct posix_thread *child, *parent = NULL;
	struct uk_sigaction *previous;
	int rc;

	UK_ASSERT(event && event->cl_args && event->child);
	uk_mutex_lock(&pprocess_mutex);
	child = tid2pthread(ukthread2tid(event->child));
	UK_ASSERT(child);
	if (event->parent) {
		parent = tid2pthread(ukthread2tid(event->parent));
		UK_ASSERT(parent);
	}
	rc = pprocess_signal_inherit_locked(child, parent,
					    event->cl_args->flags, &previous);
	uk_mutex_unlock(&pprocess_mutex);
	if (previous)
		pprocess_signal_sigaction_release(previous);
	return rc;
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
static int uk_posix_clone_sighand(void *arg)
{
	struct posix_process_clone_event_data *event_data;
	const struct clone_args *cl_args;

	event_data = (struct posix_process_clone_event_data *)arg;
	UK_ASSERT(event_data);
	UK_ASSERT(event_data->cl_args);

	cl_args = event_data->cl_args;

	/* CLONE_SIGHAND and CLONE_CLEAR_SIGHAND should not be together */
	if (unlikely((cl_args->flags & (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)) ==
		     (CLONE_SIGHAND | CLONE_CLEAR_SIGHAND)))
		return -EINVAL;
	/* CLONE_SIGHAND requires CLONE_VM */
	if (unlikely((cl_args->flags & CLONE_SIGHAND) &&
		     !(cl_args->flags & CLONE_VM)))
		return -EINVAL;
	/* CLONE_THREAD requires CLONE_SIGHAND */
	if (unlikely((cl_args->flags & CLONE_THREAD) &&
		     !(cl_args->flags & CLONE_SIGHAND)))
		return -EINVAL;

	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */

POSIX_PROCESS_CLONE_HANDLER(CLONE_SIGHAND | CLONE_CLEAR_SIGHAND,
			    uk_posix_clone_sighand);
