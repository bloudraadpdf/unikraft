/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Convenience filesystem operations */

#include <uk/errptr.h>
#include <uk/fs/common-ops.h>
#include <uk/arch/limits.h>
#include <string.h>

struct uk_fs_path uk_fs_path_copy(const char *s, size_t len,
				  struct uk_alloc *allocator)
{
	char *copy;

	if (len == __SZ_MAX)
		return (struct uk_fs_path){ .s = ERR2PTR(-ENAMETOOLONG) };
	copy = uk_malloc(allocator, len + 1);
	if (!copy)
		return (struct uk_fs_path){ .s = ERR2PTR(-ENOMEM) };
	if (len)
		memcpy(copy, s, len);
	copy[len] = '\0';
	return (struct uk_fs_path){
		.s = copy, .len = len, .allocator = allocator
	};
}

void uk_fs_path_release(struct uk_fs_path path)
{
	if (path.allocator)
		uk_free(path.allocator, (void *)path.s);
}

int uk_fs_common_nop_sync(const struct uk_file *f __unused)
{
	return 0;
}

const struct uk_file *uk_fs_common_nop_rebind(const struct uk_file *f __unused,
					      unsigned long flags __unused,
					      const void *data __unused)
{
	return ERR2PTR(-ENOSYS);
}

int uk_fs_common_nop_graft(const struct uk_file *f __unused,
			   const struct uk_file *ref __unused)
{
	return -ENOSYS;
}

/* Operations for read-only filesystems, return -EROFS */
const struct uk_file *uk_fs_common_rofs_create(const struct uk_file *f __unused,
					       const char *name __unused,
					       size_t len __unused,
					       unsigned int mode __unused,
					       int flags __unused,
					       union uk_fs_create_target target
					       __unused)
{
	return ERR2PTR(-EROFS);
}

int uk_fs_common_rofs_unlink(const struct uk_file *f __unused,
			     const char *name __unused, size_t len __unused,
			     unsigned int flags __unused)
{
	return -EROFS;
}

int uk_fs_common_rofs_rename(const struct uk_file *f __unused,
			     const char *name __unused, size_t nlen __unused,
			     const struct uk_file *dest __unused,
			     const char *dname __unused, size_t dlen __unused,
			     unsigned int flags __unused)
{
	return -EROFS;
}
