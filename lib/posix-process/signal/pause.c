/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2024, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <stddef.h>
#include <sys/types.h>
#include <uk/syscall.h>

#include "process.h"
#include "signal.h"

#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
UK_SYSCALL_R_DEFINE(int, pause)
{
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	rc = pprocess_signal_wait_locked(uk_pthread_current(), NULL, NULL, 0);
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}
#else /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
UK_SYSCALL_R_DEFINE(int, pause)
{
	UK_WARN_STUBBED();
	return 0;
}
#endif /* !CONFIG_LIBPOSIX_PROCESS_SIGNAL */
