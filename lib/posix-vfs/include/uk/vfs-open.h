/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_VFS_OPEN_H
#define UK_VFS_OPEN_H

#include <uk/arch/types.h>

#define UK_VFS_RESOLVE_NO_XDEV 0x01
#define UK_VFS_RESOLVE_NO_MAGICLINKS 0x02
#define UK_VFS_RESOLVE_NO_SYMLINKS 0x04
#define UK_VFS_RESOLVE_BENEATH 0x08
#define UK_VFS_RESOLVE_IN_ROOT 0x10
#define UK_VFS_RESOLVE_CACHED 0x20
#define UK_VFS_RESOLVE_SCOPED (UK_VFS_RESOLVE_BENEATH | UK_VFS_RESOLVE_IN_ROOT)

struct uk_vfs_open_how {
	__u64 flags;
	__u64 mode;
	__u64 resolve;
};

int uk_posix_vfs_open_validate(const struct uk_vfs_open_how *how);

#endif
