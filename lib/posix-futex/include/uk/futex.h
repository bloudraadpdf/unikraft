/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_FUTEX_H__
#define __UK_FUTEX_H__

#include <stdbool.h>
#include <uk/arch/types.h>

bool uk_futex_has_waiters(__u32 *address);

#endif
