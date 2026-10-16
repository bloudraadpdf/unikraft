/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/arch/limits.h>
#include <uk/nofault.h>
#include <uk/process.h>
#include <uk/syscall.h>
#include "process.h"

#if CONFIG_LIBPOSIX_FDTAB
static struct rlimit initial_nofile = {
	CONFIG_LIBPOSIX_FDTAB_MAXFDS, CONFIG_LIBPOSIX_FDTAB_MAXFDS
};

unsigned int uk_posix_nofile_limit(void)
{
	unsigned int limit = initial_nofile.rlim_cur;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *thread = uk_pthread_current();

	if (thread) {
		uk_mutex_lock(&pprocess_mutex);
		limit = thread->process->nofile_limit.rlim_cur;
		uk_mutex_unlock(&pprocess_mutex);
	}
#endif
	return limit;
}
#endif

int uk_sys_prlimit64(int pid, unsigned int resource,
		     const struct rlimit *new_limit, struct rlimit *old_limit)
{
	struct rlimit previous, requested;
	struct rlimit *stored = NULL;
	int result = 0;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *thread;
	struct posix_process *process;
#endif

	switch (resource) {
	case RLIMIT_AS:
	case RLIMIT_DATA:
		previous = (struct rlimit) { RLIM_INFINITY, RLIM_INFINITY };
		break;
	case RLIMIT_STACK:
		previous = (struct rlimit) { __STACK_SIZE, __STACK_SIZE };
		break;
#if CONFIG_LIBPOSIX_FDTAB
	case RLIMIT_NOFILE:
		stored = &initial_nofile;
		break;
#endif
	default:
		return -EINVAL;
	}
	if (new_limit) {
		if (!uk_nofault_try_read(requested, new_limit))
			return -EFAULT;
		if (requested.rlim_cur > requested.rlim_max)
			return -EINVAL;
	}
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_lock(&pprocess_mutex);
	thread = uk_pthread_current();
	process = pid ? tid2pprocess(pid) : (thread ? thread->process : NULL);
	if (pid && !process) {
		result = -ESRCH;
		goto out;
	}
#if CONFIG_LIBPOSIX_FDTAB
	if (resource == RLIMIT_NOFILE && process)
		stored = &process->nofile_limit;
#endif
#else
	if (pid && pid != uk_sys_getpid())
		return -ESRCH;
#endif
	if (stored)
		previous = *stored;
	if (new_limit) {
		if (!stored) {
			result = -EOPNOTSUPP;
			goto out;
		}
#if CONFIG_LIBPOSIX_FDTAB
		if (requested.rlim_max > CONFIG_LIBPOSIX_FDTAB_MAXFDS) {
			result = -EPERM;
			goto out;
		}
#endif
#if !CONFIG_LIBPOSIX_USER || CONFIG_LIBPOSIX_USER_UID != 0
		if (requested.rlim_max > previous.rlim_max) {
			result = -EPERM;
			goto out;
		}
#endif
		*stored = requested;
	}
	if (old_limit && !uk_nofault_try_write(previous, old_limit))
		result = -EFAULT;
out:
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_unlock(&pprocess_mutex);
#endif
	return result;
}

UK_LLSYSCALL_R_DEFINE(int, prlimit64, int, pid, unsigned int, resource,
		      const struct rlimit *, new_limit, struct rlimit *, old_limit)
{
	return uk_sys_prlimit64(pid, resource, new_limit, old_limit);
}

UK_SYSCALL_R_DEFINE(int, getrlimit, int, resource, struct rlimit *, limit)
{
	return uk_sys_getrlimit(resource, limit);
}

UK_SYSCALL_R_DEFINE(int, setrlimit, int, resource, const struct rlimit *, limit)
{
	return uk_sys_setrlimit(resource, limit);
}

#if UK_LIBC_SYSCALLS
int prlimit(pid_t pid, int resource, const struct rlimit *new_limit,
	    struct rlimit *old_limit)
{
	return uk_syscall_e_prlimit64(pid, resource, (long)new_limit,
				    (long)old_limit);
}
#endif
