/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/credentials.h>
#include <uk/nofault.h>
#include <uk/process.h>
#include <uk/syscall.h>
#include "process.h"

#define CAP_VERSION_1 0x19980330U
#define CAP_VERSION_2 0x20071026U
#define CAP_VERSION_3 0x20080522U

struct cap_header {
	__u32 version;
	__s32 pid;
};

struct cap_data {
	__u32 effective, permitted, inheritable;
};

UK_CTASSERT(sizeof(struct cap_header) == 8);
UK_CTASSERT(sizeof(struct cap_data) == 12);

static int capability_version(void *header, unsigned int *words)
{
	__u32 version;

	if (!uk_nofault_try_read(version, header))
		return -EFAULT;
	switch (version) {
	case CAP_VERSION_1:
		*words = 1;
		return 0;
	case CAP_VERSION_2:
	case CAP_VERSION_3:
		*words = 2;
		return 0;
	default:
		version = CAP_VERSION_3;
		return uk_nofault_try_write(version, header) ?
			-EINVAL : -EFAULT;
	}
}

static struct pu_credentials *capability_target(__s32 pid)
{
	struct pu_credentials *credentials = NULL;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct uk_thread *thread;
#endif

	if (!pid)
		return pu_credentials_acquire(uk_thread_current(), NULL);
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	uk_mutex_lock(&pprocess_mutex);
	thread = tid2ukthread(pid);
	if (thread && (thread->flags & UK_THREADF_UKTLS))
		credentials = pu_credentials_acquire(thread, NULL);
	uk_mutex_unlock(&pprocess_mutex);
#else
	if (pid == uk_sys_gettid())
		credentials = pu_credentials_acquire(uk_thread_current(), NULL);
#endif
	return credentials;
}

UK_SYSCALL_R_DEFINE(int, capget, struct cap_header *, header,
		    struct cap_data *, data)
{
	struct pu_credentials *credentials;
	const struct pu_privileges *privileges;
	struct cap_data output[2];
	unsigned int words, i;
	__s32 pid;
	int result;

	result = capability_version(header, &words);
	if (result && result != -EINVAL)
		return result;
	if (!data)
		return 0;
	if (result)
		return result;
	if (!uk_nofault_try_read(pid, &header->pid))
		return -EFAULT;
	if (pid < 0)
		return -EINVAL;
	credentials = capability_target(pid);
	if (!credentials)
		return -ESRCH;
	privileges = pu_credentials_privileges(credentials);
	for (i = 0; i < words; i++) {
		output[i].effective = privileges->effective >> (i * 32);
		output[i].permitted = privileges->permitted >> (i * 32);
		output[i].inheritable = privileges->inheritable >> (i * 32);
	}
	result = uk_nofault_memcpy((char *)data, (const char *)output,
				   words * sizeof(*data), 0) ==
		words * sizeof(*data) ? 0 : -EFAULT;
	pu_credentials_release(credentials);
	return result;
}

UK_SYSCALL_R_DEFINE(int, capset, struct cap_header *, header,
		    const struct cap_data *, data)
{
	struct pu_credentials *credentials, *replacement, *previous;
	struct cap_data input[2];
	__u64 effective = 0, permitted = 0, inheritable = 0;
	unsigned int words, i;
	__s32 pid;
	int result;

	result = capability_version(header, &words);
	if (result)
		return result;
	if (!uk_nofault_try_read(pid, &header->pid))
		return -EFAULT;
	if (pid < 0 || (pid && pid != uk_sys_gettid()))
		return -EPERM;
	if (uk_nofault_memcpy((char *)input, (const char *)data,
			      words * sizeof(*data), 0) !=
	    words * sizeof(*data))
		return -EFAULT;
	for (i = 0; i < words; i++) {
		effective |= (__u64)input[i].effective << (i * 32);
		permitted |= (__u64)input[i].permitted << (i * 32);
		inheritable |= (__u64)input[i].inheritable << (i * 32);
	}
	credentials = pu_credentials_acquire(uk_thread_current(), NULL);
	if (!credentials)
		return -ESRCH;
	result = pu_credentials_capabilities(uk_alloc_get_default(),
					     credentials, effective, permitted,
					     inheritable, &replacement);
	if (!result) {
		previous = pu_credentials_exchange(replacement);
		pu_credentials_release(previous);
	}
	pu_credentials_release(credentials);
	return result;
}
