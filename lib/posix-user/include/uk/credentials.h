/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_CREDENTIALS_H__
#define __UK_CREDENTIALS_H__

#include <stdbool.h>
#include <stdint.h>
#include <uk/alloc.h>
#include <uk/thread.h>

#define PU_CAP_LAST 40
#define PU_CAP_KNOWN_MASK ((UINT64_C(1) << (PU_CAP_LAST + 1)) - 1)
#define PU_CAP_SETGID (UINT64_C(1) << 6)
#define PU_CAP_SETUID (UINT64_C(1) << 7)
#define PU_CAP_SETPCAP (UINT64_C(1) << 8)
#define PU_CAP_SYS_NICE (UINT64_C(1) << 23)
#define PU_CAP_SYS_RESOURCE (UINT64_C(1) << 24)
#define PU_CAP_SYS_TIME (UINT64_C(1) << 25)
#define PU_CAP_CHOWN (UINT64_C(1) << 0)
#define PU_CAP_DAC_OVERRIDE (UINT64_C(1) << 1)
#define PU_CAP_DAC_READ_SEARCH (UINT64_C(1) << 2)
#define PU_CAP_FOWNER (UINT64_C(1) << 3)
#define PU_CAP_FSETID (UINT64_C(1) << 4)
#define PU_CAP_LINUX_IMMUTABLE (UINT64_C(1) << 9)
#define PU_CAP_MKNOD (UINT64_C(1) << 27)
#define PU_CAP_MAC_OVERRIDE (UINT64_C(1) << 32)
#define PU_CAP_FS_SET (PU_CAP_CHOWN | PU_CAP_DAC_OVERRIDE | \
	PU_CAP_DAC_READ_SEARCH | PU_CAP_FOWNER | PU_CAP_FSETID | \
	PU_CAP_LINUX_IMMUTABLE | PU_CAP_MKNOD | PU_CAP_MAC_OVERRIDE)

#define PU_SECURE_NOROOT (UINT32_C(1) << 0)
#define PU_SECURE_NOROOT_LOCKED (UINT32_C(1) << 1)
#define PU_SECURE_NO_SETUID_FIXUP (UINT32_C(1) << 2)
#define PU_SECURE_NO_SETUID_FIXUP_LOCKED (UINT32_C(1) << 3)
#define PU_SECURE_KEEP_CAPS (UINT32_C(1) << 4)
#define PU_SECURE_KEEP_CAPS_LOCKED (UINT32_C(1) << 5)
#define PU_SECURE_NO_AMBIENT_RAISE (UINT32_C(1) << 6)
#define PU_SECURE_NO_AMBIENT_RAISE_LOCKED (UINT32_C(1) << 7)
#define PU_SECURE_EXEC_RESTRICT_FILE (UINT32_C(1) << 8)
#define PU_SECURE_EXEC_RESTRICT_FILE_LOCKED (UINT32_C(1) << 9)
#define PU_SECURE_EXEC_DENY_INTERACTIVE (UINT32_C(1) << 10)
#define PU_SECURE_EXEC_DENY_INTERACTIVE_LOCKED (UINT32_C(1) << 11)
#define PU_SECURE_ALL_BITS (PU_SECURE_NOROOT | PU_SECURE_NO_SETUID_FIXUP | \
	PU_SECURE_KEEP_CAPS | PU_SECURE_NO_AMBIENT_RAISE | \
	PU_SECURE_EXEC_RESTRICT_FILE | PU_SECURE_EXEC_DENY_INTERACTIVE)
#define PU_SECURE_ALL_LOCKS (PU_SECURE_ALL_BITS << 1)
#define PU_SECURE_UNPRIVILEGED (PU_SECURE_EXEC_RESTRICT_FILE | \
	PU_SECURE_EXEC_DENY_INTERACTIVE)

struct pu_credentials;

struct pu_identity {
	__u32 real_uid, effective_uid, saved_uid, fs_uid;
	__u32 real_gid, effective_gid, saved_gid, fs_gid;
};

struct pu_privileges {
	__u64 effective, permitted, inheritable, ambient, bounding;
	unsigned int securebits;
};

const struct pu_identity *
pu_credentials_identity(const struct pu_credentials *credentials);
const struct pu_privileges *
pu_credentials_privileges(const struct pu_credentials *credentials);
size_t pu_credentials_group_count(const struct pu_credentials *credentials);
const __u32 *
pu_credentials_group_ids(const struct pu_credentials *credentials);
bool pu_credentials_has_group(const struct pu_credentials *credentials,
			      __u32 group);

struct pu_credentials *pu_credentials_acquire(struct uk_thread *thread,
					      bool *no_new_privs);
struct pu_credentials *
pu_credentials_retain(struct pu_credentials *credentials);
int pu_no_new_privs_set(void);
void pu_credentials_release(struct pu_credentials *credentials);
struct pu_credentials *
pu_credentials_copy(struct uk_alloc *allocator,
		    const struct pu_credentials *credentials);
int pu_credentials_capabilities(struct uk_alloc *allocator,
				const struct pu_credentials *credentials,
				__u64 effective, __u64 permitted,
				__u64 inheritable,
				struct pu_credentials **replacement);

/* Transfer to active UKTLS and return its old reference; NULL detaches. */
struct pu_credentials *
pu_credentials_exchange(struct pu_credentials *credentials);

#endif /* __UK_CREDENTIALS_H__ */
