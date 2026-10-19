/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <string.h>
#include <uk/arch/limits.h>

#include <uk/file/nops.h>


ssize_t uk_file_nop_read(const struct uk_file *f __unused,
			 const struct iovec *iov __unused,
			 size_t iovcnt __unused,
			 size_t off __unused, long flags __unused)
{
	return -ENOSYS;
}

struct uk_file_io_result
uk_file_nop_write(const struct uk_file *f __unused,
		  const struct iovec *iov __unused, size_t iovcnt __unused,
		  size_t off __unused, long flags __unused,
		  enum uk_file_write_kind kind __unused)
{
	return uk_file_io_status(-ENOSYS);
}

ssize_t uk_file_nop_mem(const struct uk_file *f __unused,
			enum uk_file_mem_op op __unused,
			size_t off __unused, size_t len __unused,
			struct iovec *iov __unused, size_t iovcnt __unused)
{
	return -ENODEV;
}

int uk_file_nop_getstat(const struct uk_file *f __unused,
			unsigned mask __unused, struct uk_statx *arg __unused)
{
	return -ENOSYS;
}

int uk_file_nop_setstat(const struct uk_file *f __unused,
			unsigned mask __unused,
			const struct uk_statx *arg __unused)
{
	return -ENOSYS;
}

int uk_file_nop_ctl(const struct uk_file *f __unused, int fam,
		    int req __unused, uintptr_t arg1 __unused,
		    uintptr_t arg2 __unused, uintptr_t arg3 __unused)
{
	return fam == UKFILE_CTL_IOCTL ? -ENOTTY : -ENOSYS;
}

const struct uk_file_ops uk_file_nops =
	UK_FILE_NOP_OPS(uk_file_nop_getstat);

void uk_file_static_release(const struct uk_file *f __maybe_unused,
			    int what __unused)
{
	uk_pr_warn("Destructor called on static file: %p\n", f);
}

ssize_t uk_file_anon_read(const struct uk_file *file __unused,
			  const struct iovec *iov __unused,
			  size_t count __unused, size_t offset __unused,
			  long flags __unused)
{
	return -EINVAL;
}

struct uk_file_io_result
uk_file_anon_write(const struct uk_file *file __unused,
		   const struct iovec *iov __unused, size_t count __unused,
		   size_t offset __unused, long flags __unused,
		   enum uk_file_write_kind kind __unused)
{
	return uk_file_io_status(-EINVAL);
}

int uk_file_anon_getstat(const struct uk_file *file __unused,
			 unsigned int mask __unused, struct uk_statx *stat)
{
	static const char inode;

	memset(stat, 0, sizeof(*stat));
	stat->stx_mask = UK_STATX_TYPE | UK_STATX_MODE | UK_STATX_NLINK |
		UK_STATX_UID | UK_STATX_GID | UK_STATX_INO | UK_STATX_SIZE |
		UK_STATX_BLOCKS;
	stat->stx_mode = 0600;
	stat->stx_nlink = 1;
	stat->stx_ino = (uintptr_t)&inode;
	stat->stx_blksize = __PAGE_SIZE;
	return 0;
}

const struct uk_file_ops uk_file_anon_ops = {
	.read = uk_file_anon_read,
	.write = uk_file_anon_write,
	.mem = uk_file_nop_mem,
	.getstat = uk_file_anon_getstat,
	.setstat = uk_file_nop_setstat,
	.ctl = uk_file_nop_ctl
};
