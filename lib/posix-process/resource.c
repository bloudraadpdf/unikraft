/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <string.h>
#include <uk/arch/limits.h>
#include <uk/nofault.h>
#include <uk/process.h>
#include <uk/syscall.h>
#if CONFIG_LIBPOSIX_USER
#include <uk/credentials.h>
#endif
#include "process.h"

static struct rlimit initial_limits[RLIM_NLIMITS] = {
	[RLIMIT_AS] = { RLIM_INFINITY, RLIM_INFINITY },
	[RLIMIT_DATA] = { RLIM_INFINITY, RLIM_INFINITY },
#if CONFIG_LIBPOSIX_PROCESS_EXECVE && CONFIG_LIBUKALLOCSTACK_PAGE_GUARDS
	[RLIMIT_STACK] = { CONFIG_LIBPOSIX_PROCESS_STACK_SIZE, RLIM_INFINITY },
#else
	[RLIMIT_STACK] = { __STACK_SIZE, __STACK_SIZE },
#endif
#if CONFIG_LIBPOSIX_FDTAB
	[RLIMIT_NOFILE] = {
		CONFIG_LIBPOSIX_FDTAB_MAXFDS, CONFIG_LIBPOSIX_FDTAB_MAXFDS
	},
#endif
};

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
void pprocess_resource_init(struct posix_process *process,
			    struct posix_process *parent)
{
	uk_mutex_lock(&pprocess_mutex);
	memcpy(process->limits, parent ? parent->limits : initial_limits,
	       sizeof(process->limits));
	uk_mutex_unlock(&pprocess_mutex);
}
#endif

#if CONFIG_LIBPOSIX_FDTAB
unsigned int uk_posix_nofile_limit(void)
{
	unsigned int limit = initial_limits[RLIMIT_NOFILE].rlim_cur;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *thread = uk_pthread_current();

	if (thread) {
		uk_mutex_lock(&pprocess_mutex);
		limit = thread->process->limits[RLIMIT_NOFILE].rlim_cur;
		uk_mutex_unlock(&pprocess_mutex);
	}
#endif
	return limit;
}
#endif

static bool resource_writable(unsigned int resource)
{
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	if (resource == RLIMIT_RTPRIO)
		return true;
#endif
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBUKVMEM && \
	CONFIG_LIBPOSIX_MMAP
#if CONFIG_LIBUKALLOCSTACK_PAGE_GUARDS
	return resource < RLIM_NLIMITS;
#else
	return resource != RLIMIT_STACK;
#endif
#else
	return resource == RLIMIT_NOFILE;
#endif
}

int uk_sys_prlimit64(int pid, unsigned int resource,
		     const struct rlimit *new_limit, struct rlimit *old_limit)
{
	struct rlimit previous, requested;
	struct rlimit *stored;
	int result = 0;
#if CONFIG_LIBPOSIX_USER
	struct pu_credentials *credentials = NULL;
#endif
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *thread;
	struct posix_process *process;
#endif

	switch (resource) {
	case RLIMIT_AS:
	case RLIMIT_DATA:
	case RLIMIT_STACK:
		break;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	case RLIMIT_RTPRIO:
		break;
#endif
#if CONFIG_LIBPOSIX_FDTAB
	case RLIMIT_NOFILE:
		break;
#endif
	default:
		return -EINVAL;
	}
	stored = &initial_limits[resource];
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
	if (process)
		stored = &process->limits[resource];
#else
	if (pid && pid != uk_sys_getpid())
		return -ESRCH;
#endif
	previous = *stored;
	if (new_limit) {
		if (!resource_writable(resource)) {
			result = -EOPNOTSUPP;
			goto out;
		}
#if CONFIG_LIBPOSIX_FDTAB
		if (resource == RLIMIT_NOFILE &&
		    requested.rlim_max > CONFIG_LIBPOSIX_FDTAB_MAXFDS) {
			result = -EPERM;
			goto out;
		}
#endif
		if (requested.rlim_max > previous.rlim_max) {
#if CONFIG_LIBPOSIX_USER
			struct uk_thread *caller = uk_thread_current();

			if (caller && (caller->flags & UK_THREADF_UKTLS))
				credentials = pu_credentials_acquire(caller, NULL);
			if (!credentials) {
				result = -ESRCH;
				goto out;
			}
			if (!(pu_credentials_privileges(credentials)->effective &
			      PU_CAP_SYS_RESOURCE)) {
				result = -EPERM;
				goto out;
			}
#else
			result = -EPERM;
			goto out;
#endif
		}
		*stored = requested;
	}
	if (old_limit && !uk_nofault_try_write(previous, old_limit))
		result = -EFAULT;
out:
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_unlock(&pprocess_mutex);
#endif
#if CONFIG_LIBPOSIX_USER
	if (credentials)
		pu_credentials_release(credentials);
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
