/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <sys/membarrier.h>
#include <uk/alloc.h>
#include <uk/sched.h>
#include <uk/syscall.h>
#include "process.h"

#define MEMBARRIER_SUPPORTED_CMDS (\
	MEMBARRIER_CMD_GLOBAL | \
	MEMBARRIER_CMD_GLOBAL_EXPEDITED | \
	MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | \
	MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED | \
	MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE | \
	MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)

UK_LLSYSCALL_R_DEFINE(int, membarrier, int, cmd, unsigned int, flags,
		      int __unused, cpu_id)
{
	unsigned int required = 0;
	struct posix_process *process;
	bool sync_core = false;
	int rc;

	if (flags)
		return -EINVAL;
	switch (cmd) {
	case MEMBARRIER_CMD_QUERY:
		return MEMBARRIER_SUPPORTED_CMDS;
	case MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
	case MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
		rc = uk_sched_membarrier(false);
		if (rc)
			return rc;
		uk_mutex_lock(&pprocess_mutex);
		uk_pprocess_current()->vm_context->membarrier_registered |= cmd;
		uk_mutex_unlock(&pprocess_mutex);
		return 0;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED:
		required = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED;
		break;
	case MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
		required = MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE;
		sync_core = true;
		break;
	case MEMBARRIER_CMD_GLOBAL:
	case MEMBARRIER_CMD_GLOBAL_EXPEDITED:
		break;
	default:
		return -EINVAL;
	}
	if (required) {
		uk_mutex_lock(&pprocess_mutex);
		process = uk_pprocess_current();
		rc = process->vm_context->membarrier_registered & required ? 0 : -EPERM;
		uk_mutex_unlock(&pprocess_mutex);
		if (rc)
			return rc;
	}
	return uk_sched_membarrier(sync_core);
}
