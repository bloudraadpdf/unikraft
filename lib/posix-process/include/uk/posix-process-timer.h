/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_POSIX_PROCESS_TIMER_H
#define UK_POSIX_PROCESS_TIMER_H
#include <time.h>
#include <sys/time.h>

int uk_posix_timer_create(clockid_t clock, const void *event, int *id);
int uk_posix_timer_delete(int id);
int uk_posix_timer_settime(int id, int flags, const struct itimerspec *value,
			   struct itimerspec *old);
int uk_posix_timer_gettime(int id, struct itimerspec *value);
int uk_posix_timer_getoverrun(int id);
int uk_posix_setitimer(int which, const struct itimerval *value,
		       struct itimerval *old);
int uk_posix_getitimer(int which, struct itimerval *value);
unsigned int uk_posix_alarm(unsigned int seconds);
void uk_posix_timers_clock_changed(void);
#endif
