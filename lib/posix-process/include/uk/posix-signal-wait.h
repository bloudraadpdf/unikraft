/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_POSIX_SIGNAL_WAIT_H__
#define __UK_POSIX_SIGNAL_WAIT_H__

#include <signal.h>
#include <stdbool.h>
#include <uk/atomic.h>
#include <uk/wait.h>

struct posix_thread;

struct uk_posix_signal_wait {
	struct posix_thread *thread;
	struct uk_waitq *queue; /* NULL selects direct thread wake-up. */
	unsigned long saved_mask;
	unsigned int interrupted;
	bool masked;
};

int uk_posix_signal_wait_init(struct uk_posix_signal_wait *wait,
			      struct uk_waitq *queue,
			      const sigset_t *mask, size_t size);
int uk_posix_signal_wait_check(struct uk_posix_signal_wait *wait);
void uk_posix_signal_wait_end(struct uk_posix_signal_wait *wait, int result);

#endif
