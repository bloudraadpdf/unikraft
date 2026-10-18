/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Internal syscalls for file control operations */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <linux/falloc.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <uk/atomic.h>
#include <uk/nofault.h>
#include <uk/posix-fdio.h>
#include <uk/print.h>
#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#endif
#if CONFIG_LIBPOSIX_PROCESS
#include <uk/process.h>
#endif

#include "fdio-impl.h"

/* Needed for IOCTL_CMD_ISTYPE */
#include <vfscore/vnode.h>

int uk_sys_ioctl(struct uk_ofile *of, int cmd, void *arg)
{
	int r;
	int iolock;
	const struct uk_file *f;

	if (cmd == FIONBIO) {
		int val;

		if (!uk_nofault_try_read(val, (int *)arg))
			return -EFAULT;

		if (val)
			uk_or(&of->mode, O_NONBLOCK);
		else
			uk_and(&of->mode, ~O_NONBLOCK);
		return 0;
	}

	iolock = _SHOULD_LOCK(of->mode);
	f = of->file;
	if (iolock)
		uk_file_wlock(f);
	r = uk_file_ctl(f, UKFILE_CTL_IOCTL, cmd, (uintptr_t)arg, 0, 0);
	if (iolock)
		uk_file_wunlock(f);

	/* Universally return -ENOTTY for unimplemented TTY ioctls */
	if (unlikely(r == -ENOSYS && IOCTL_CMD_ISTYPE(cmd, IOCTL_CMD_TYPE_TTY)))
		return -ENOTTY;
	return r;
}

/* File flags that fcntl can change, according to POSIX */
#define _FCNTL_MASK (O_APPEND|O_ASYNC|O_DIRECT|O_NOATIME|O_NONBLOCK)
/* File flags we actually support & care about changing */
#define _SETFL_MASK (_FCNTL_MASK & UKFD_MODE_MASK)

static int file_record_lock(struct uk_ofile *of, int cmd, struct flock *flock)
{
	struct uk_file_lock_request request = { .file = of->file };
	const int description = cmd == F_OFD_SETLK || cmd == F_OFD_SETLKW || cmd == F_OFD_GETLK;
	const int query = cmd == F_GETLK || cmd == F_OFD_GETLK;
	off_t base = 0;

	if (!flock)
		return -EFAULT;
	if (of->mode & O_PATH)
		return -EBADF;
	if (flock->l_type != F_RDLCK && flock->l_type != F_WRLCK &&
	    (query || flock->l_type != F_UNLCK))
		return -EINVAL;
	if (description && flock->l_pid)
		return -EINVAL;
	if (!query && ((flock->l_type == F_RDLCK && !_CAN_READ(of->mode)) ||
		       (flock->l_type == F_WRLCK && !fdio_can_write(of->mode))))
		return -EBADF;
	switch (flock->l_whence) {
	case SEEK_SET:
		break;
	case SEEK_CUR:
		uk_mutex_lock(&of->lock);
		if (of->pos > LLONG_MAX) {
			uk_mutex_unlock(&of->lock);
			return -EOVERFLOW;
		}
		base = of->pos;
		uk_mutex_unlock(&of->lock);
		break;
	case SEEK_END: {
		struct uk_statx stat;
		uk_file_rlock(of->file);
		int rc = uk_file_getstat(of->file, UK_STATX_SIZE, &stat);
		uk_file_runlock(of->file);
		if (rc)
			return rc;
		if (stat.stx_size > LLONG_MAX)
			return -EOVERFLOW;
		base = stat.stx_size;
		break;
	}
	default:
		return -EINVAL;
	}
	if (flock->l_start > 0 && base > LLONG_MAX - flock->l_start)
		return -EOVERFLOW;
	request.start = base + flock->l_start;
	if (request.start < 0)
		return -EINVAL;
	request.end = LLONG_MAX;
	if (flock->l_len > 0) {
		if (flock->l_len - 1 > LLONG_MAX - request.start)
			return -EOVERFLOW;
		request.end = request.start + flock->l_len - 1;
	} else if (flock->l_len < 0) {
		if (flock->l_len < -request.start)
			return -EINVAL;
		request.end = request.start - 1;
		request.start += flock->l_len;
	}
	request.type = flock->l_type;
	request.kind = description ? UK_FILE_LOCK_DESCRIPTION : UK_FILE_LOCK_PROCESS;
	if (description) {
		request.owner = of;
		request.pid = -1;
	} else {
#if CONFIG_LIBPOSIX_FDTAB
		request.owner = uk_fdtab_lock_owner();
#else
		return -ENOSYS;
#endif
#if CONFIG_LIBPOSIX_PROCESS
		request.pid = uk_sys_getpid();
#endif
	}
	return uk_file_lock_apply(&request, query ? flock : NULL,
				  cmd == F_SETLKW || cmd == F_OFD_SETLKW);
}

int uk_sys_fcntl(struct uk_ofile *of, int cmd, unsigned long arg)
{
	switch (cmd) {
	case F_GETFL:
		return of->mode & UKFD_MODE_MASK;
	case F_SETFL:
	{
		unsigned int mode = of->mode;
		unsigned int newmode;

		if (mode & O_PATH)
			return -EBADF;
		do {
			newmode = mode & ~_SETFL_MASK;
			newmode |= arg & _SETFL_MASK;
		} while (!uk_compare_exchange_n(&of->mode, &mode, newmode));
		return 0;
	}
	case F_SETLK:
	case F_SETLKW:
	case F_GETLK:
	case F_OFD_SETLK:
	case F_OFD_SETLKW:
	case F_OFD_GETLK:
		return file_record_lock(of, cmd, (struct flock *)arg);
	default:
		uk_pr_warn("STUB: fcntl(%d)\n", cmd);
		return -EINVAL;
	}
}

int uk_sys_flock(struct uk_ofile *of, int cmd)
{
	struct uk_file_lock_request request = {
		.file = of->file, .owner = of, .kind = UK_FILE_LOCK_FLOCK,
		.start = 0, .end = LLONG_MAX, .pid = -1
	};
	if (of->mode & O_PATH)
		return -EBADF;
	switch (cmd & ~LOCK_NB) {
	case LOCK_SH: request.type = F_RDLCK; break;
	case LOCK_EX: request.type = F_WRLCK; break;
	case LOCK_UN: request.type = F_UNLCK; break;
	default: return -EINVAL;
	}
	return uk_file_lock_apply(&request, NULL, !(cmd & LOCK_NB));
}

int uk_sys_sync_file_range(struct uk_ofile *of, off_t offset, off_t length,
			   unsigned int flags)
{
	struct uk_statx status = {0};
	struct uk_file_sync_range range;
	int rc;

	if (of->mode & O_PATH)
		return -EBADF;
	if (flags & ~(SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE |
		      SYNC_FILE_RANGE_WAIT_AFTER) ||
	    offset < 0 || length < 0 || length > LLONG_MAX - offset)
		return -EINVAL;
	uk_file_rlock(of->file);
	rc = uk_file_getstat(of->file, UK_STATX_TYPE, &status);
	uk_file_runlock(of->file);
	if (rc)
		return rc;
	if (!S_ISREG(status.stx_mode) && !S_ISDIR(status.stx_mode) &&
	    !S_ISBLK(status.stx_mode))
		return -ESPIPE;
	if (!flags)
		return 0;
	range = (struct uk_file_sync_range){.start = offset,
					    .end = length ? offset + length - 1
							  : LLONG_MAX,
					    .flags = flags};
	return uk_file_ctl(of->file, UKFILE_CTL_FILE,
			   UKFILE_CTL_FILE_SYNC_RANGE, (uintptr_t)&range, 0, 0);
}

int uk_sys_fsync(struct uk_ofile *of)
{
	return uk_file_ctl(of->file, UKFILE_CTL_FILE, UKFILE_CTL_FILE_SYNC,
			   -1, 0, 0);
}

int uk_sys_fdatasync(struct uk_ofile *of)
{
	return uk_file_ctl(of->file, UKFILE_CTL_FILE, UKFILE_CTL_FILE_SYNC,
			   0, 0, 0);
}

int uk_sys_ftruncate(struct uk_ofile *of, off_t len)
{
	unsigned int mode;
	const struct uk_file *f;
	int iolock;

	if (len < 0)
		return -EINVAL;

	mode = of->mode;
	if (!fdio_can_write(mode))
		return -EINVAL;

	iolock = _SHOULD_LOCK(mode);
	f = of->file;
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
	return uk_fdio_file_truncate(f, len, iolock);
#else
	int r;
	if (iolock)
		uk_file_wlock(f);
	r = uk_file_ctl(f, UKFILE_CTL_FILE, UKFILE_CTL_FILE_TRUNC,
			len, 0, 0);
	if (iolock)
		uk_file_wunlock(f);
	return r;
#endif
}

int uk_sys_fallocate(struct uk_ofile *of, int mode, off_t offset, off_t len)
{
	const struct uk_file *f;
	struct uk_statx status = {0};
	int r;
	int iolock;

	if (of->mode & O_PATH)
		return -EBADF;
	if (offset < 0 || len <= 0)
		return -EINVAL;
	switch (mode & ~FALLOC_FL_KEEP_SIZE) {
	case FALLOC_FL_ALLOCATE_RANGE:
	case FALLOC_FL_UNSHARE_RANGE:
	case FALLOC_FL_ZERO_RANGE:
		break;
	case FALLOC_FL_PUNCH_HOLE:
		if (!(mode & FALLOC_FL_KEEP_SIZE))
			return -EOPNOTSUPP;
		break;
	case FALLOC_FL_COLLAPSE_RANGE:
	case FALLOC_FL_INSERT_RANGE:
	case FALLOC_FL_WRITE_ZEROES:
		if (mode & FALLOC_FL_KEEP_SIZE)
			return -EOPNOTSUPP;
		break;
	default:
		return -EOPNOTSUPP;
	}
	if (!fdio_can_write(of->mode))
		return -EBADF;

	iolock = _SHOULD_LOCK(of->mode);
	f = of->file;
	if (iolock)
		uk_file_wlock(f);
	r = uk_file_getstat(f, UK_STATX_TYPE, &status);
	if (r)
		goto out;
	if (S_ISFIFO(status.stx_mode))
		r = -ESPIPE;
	else if (S_ISDIR(status.stx_mode))
		r = -EISDIR;
	else if (!S_ISREG(status.stx_mode) && !S_ISBLK(status.stx_mode))
		r = -ENODEV;
	else if (len > LLONG_MAX - offset)
		r = -EFBIG;
	if (r)
		goto out;
	r = uk_file_ctl(f, UKFILE_CTL_FILE, UKFILE_CTL_FILE_FALLOC,
			mode, offset, len);
out:
	if (iolock)
		uk_file_wunlock(f);
	return r;
}

int uk_sys_fadvise(struct uk_ofile *of, off_t offset, off_t len, int advice)
{
	struct uk_statx status = {0};
	int rc;

	if (of->mode & O_PATH)
		return -EBADF;
	uk_file_rlock(of->file);
	rc = uk_file_getstat(of->file, UK_STATX_TYPE, &status);
	uk_file_runlock(of->file);
	if (rc)
		return rc;
	if (S_ISFIFO(status.stx_mode))
		return -ESPIPE;
	if (len < 0)
		return -EINVAL;
	switch (advice) {
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_WILLNEED:
	case POSIX_FADV_DONTNEED:
	case POSIX_FADV_NOREUSE:
		break;
	default:
		return -EINVAL;
	}
	return uk_file_ctl(of->file, UKFILE_CTL_FILE, UKFILE_CTL_FILE_FADVISE,
			   offset, len, advice);
}
