/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <signal.h>
#include <uk/config.h>

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include "signal/signal.h"
#endif /* CONFIG_LIBPOSIX_PROCESS_SIGNAL */

#include <uk/config.h>
#include <uk/init.h>
#include <uk/process.h>

#include "process.h"

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
static void pprocess_kill_remaining(void)
{
	struct posix_process *process;
	bool found;

	uk_mutex_lock(&pprocess_mutex);
	do {
		found = false;
		uk_pprocess_foreach(process) {
			if (process->state != POSIX_PROCESS_RUNNING)
				continue;
			found = true;
			pprocess_acquire(process);
			pprocess_exit(process, POSIX_PROCESS_KILLED, SIGKILL);
			uk_mutex_unlock(&pprocess_mutex);
			uk_semaphore_down(&process->exit_semaphore);
			uk_mutex_lock(&pprocess_mutex);
			pprocess_put(process);
		}
	} while (found);
	uk_mutex_unlock(&pprocess_mutex);
}

static void pprocess_system_shutdown(struct uk_term_ctx *ctx)
{
	struct posix_process *process;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	int rc;
#endif

	uk_mutex_lock(&pprocess_mutex);
	process = pid2pprocess(UK_PID_INIT);
	if (process) {
		pprocess_acquire(process);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		if (process->state == POSIX_PROCESS_RUNNING) {
			rc = pprocess_signal_send(process, SIGTERM, NULL);
			if (rc)
				UK_CRASH("Could not signal init process (%d)\n", rc);
			uk_mutex_unlock(&pprocess_mutex);
			uk_semaphore_down(&process->exit_semaphore);
			uk_mutex_lock(&pprocess_mutex);
		}
		ctx->exit_code = process->exit_status;
#else
		ctx->exit_code = process->state == POSIX_PROCESS_RUNNING ?
			0x80 | SIGKILL : process->exit_status;
#endif
		pprocess_put(process);
	}
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_kill_remaining();
}
#else /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
/* Multithreading is not enabled. Return the exit code
 * saved by the exit_group() / _exit() stubs.
 */
static void pprocess_system_shutdown(struct uk_term_ctx *ctx)
{
	/* If we the application called one of the emulated
	 * exit syscalls, propagate the status. Otherwise,
	 * main() returned already and its return status
	 * was already set, so do not overwrite.
	 */
	if (pprocess_exit_status != PPROCESS_EXIT_STATUS_UNSET)
		ctx->exit_code = pprocess_exit_status;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

/* init last, term first */
uk_late_initcall(0, pprocess_system_shutdown);
