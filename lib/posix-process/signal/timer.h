/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSIX_PROCESS_SIGNAL_TIMER_H
#define POSIX_PROCESS_SIGNAL_TIMER_H
#include "signal.h"

void pprocess_timers_init(struct posix_process *process);
void pprocess_timers_stop_locked(struct posix_process *process, bool all,
				 struct pprocess_retired *retired);
void pprocess_timers_signal_consumed_locked(struct posix_process *process,
					    int signum);
void pprocess_timer_set_locked(struct posix_timer *timer, int flags,
			       const struct itimerspec *value, __u64 realtime,
			       __u64 monotonic);
void pprocess_timer_get_locked(struct posix_timer *timer,
			       struct itimerspec *value, __u64 realtime,
			       __u64 monotonic);
int pprocess_timer_advance_locked(struct posix_timer *timer, __u64 now,
				  enum uk_signal_dequeue_reason reason);
#endif
