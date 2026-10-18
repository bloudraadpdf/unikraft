/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __PPROCESS_SCHEDULING_H__
#define __PPROCESS_SCHEDULING_H__

#include <uk/config.h>
#if CONFIG_LIBPOSIX_USER
#include <uk/credentials.h>

int pprocess_schedule_authorize(struct uk_thread *target,
				struct pu_credentials **caller,
				struct pu_credentials **credentials);
#endif

#endif
