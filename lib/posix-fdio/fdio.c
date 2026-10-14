/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* Internal syscalls for file I/O */

#include <poll.h>
#include <string.h>
#include <uk/alloc.h>
#include <uk/arch/limits.h>
#include <uk/nofault.h>

#include <uk/posix-fdio.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif

#include "fdio-impl.h"

/* Stable mode bits to pass onto the read/write implementations */
#define _READ_MODEMASK (O_DIRECT | O_NOATIME)
#define _WRITE_MODEMASK (O_DIRECT | O_NOATIME | O_SYNC | O_DSYNC)


#define _buf2iov(buf, count) \
	((struct iovec){ .iov_base = (buf), .iov_len = (count) })

#define _SHOULD_BLOCK(r, m) ((r) == -EAGAIN && _IS_BLOCKING((m)))

/* RWF flags not supported by our libc's yet; defining them for bincompat */
#ifndef RWF_SYNC
#define RWF_SYNC	0x04
#endif /* RWF_SYNC */

#ifndef RWF_DSYNC
#define RWF_DSYNC	0x02
#endif /* RWF_DSYNC */

#ifndef RWF_APPEND
#define RWF_APPEND	0x10
#endif /* RWF_APPEND */

static inline
ssize_t fdio_get_eof(const struct uk_file *f)
{
	struct uk_statx statx;
	int r = uk_file_getstat(f, UK_STATX_SIZE, &statx);

	if (unlikely(r))
		return r;
	else
		return statx.stx_size;
}


#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
int uk_fdio_poll(const struct uk_file *file, uk_pollevent events,
		 __nsec deadline, struct uk_posix_signal_wait *wait)
{
	struct uk_pollq *q = &file->state->pollq;
	int rc;

	for (;;) {
		if (uk_file_poll_immediate(file, events))
			return 1;
		rc = uk_posix_signal_wait_check(wait);
		if (rc)
			return rc;
		if (deadline && ukplat_monotonic_clock() >= deadline)
			return 0;
		if (_uk_pollq_lock(q, events, file))
			return 1;
		(void)uk_or(&q->waitmask, events);
		uk_waitq_set_cookie(events);
		uk_waitq_wait_deadline_unless_locked(&q->waitq,
			uk_load_n(&wait->interrupted), deadline,
			uk_rwlock_rlock, uk_rwlock_runlock, &q->waitlock);
		uk_rwlock_runlock(&q->waitlock);
	}
}
#endif

static int fdio_wait(const struct uk_file *file, uk_pollevent events)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_posix_signal_wait wait;
	int rc;

	rc = uk_posix_signal_wait_init(&wait, &file->state->pollq.waitq,
				      NULL, 0);
	if (rc)
		return rc;
	rc = uk_fdio_poll(file, events, 0, &wait);
	uk_posix_signal_wait_end(&wait, rc);
	return rc < 0 ? rc : 0;
#else
	uk_file_poll(file, events);
	return 0;
#endif
}


struct fdio_buffer {
	struct iovec *user;
	struct iovec *io;
	void *data;
	int count;
};

static ssize_t fdio_buffer_copy(struct fdio_buffer *buffer, size_t length,
			       int write)
{
	size_t done = 0;

	for (int i = 0; i < buffer->count && done < length; i++) {
		size_t size = MIN(buffer->user[i].iov_len, length - done);
		char *user = buffer->user[i].iov_base;
		char *kernel = buffer->io[i].iov_base;
		size_t copied = uk_nofault_memcpy(write ? kernel : user,
				write ? user : kernel, size, 0);

		done += copied;
		if (copied != size)
			return done ? (ssize_t)done : -EFAULT;
	}
	return done;
}

static void fdio_buffer_free(struct fdio_buffer *buffer)
{
	uk_free(uk_alloc_get_default(), buffer->data);
	uk_free(uk_alloc_get_default(), buffer->user);
}

static int fdio_buffer_init(struct fdio_buffer *buffer, const struct iovec *iov,
			    int count, int write)
{
	struct uk_alloc *a = uk_alloc_get_default();
	size_t total = 0;
	size_t offset = 0;
	size_t metadata;
	int rc = -EFAULT;

	memset(buffer, 0, sizeof(*buffer));
	if (count < 0 || count > 1024)
		return -EINVAL;
	if (!count)
		return 0;
	metadata = (size_t)count * sizeof(*iov);
	buffer->user = uk_malloc(a, 2 * metadata);
	if (!buffer->user)
		return -ENOMEM;
	buffer->io = buffer->user + count;
	buffer->count = count;
	if (uk_nofault_memcpy((char *)buffer->user, (const char *)iov,
			      metadata, 0) != metadata)
		goto fail;
	for (int i = 0; i < count; i++) {
		if (buffer->user[i].iov_len > __SSZ_MAX - total) {
			rc = -EINVAL;
			goto fail;
		}
		total += buffer->user[i].iov_len;
	}
	if (total) {
		buffer->data = uk_malloc(a, total);
		if (!buffer->data) {
			rc = -ENOMEM;
			goto fail;
		}
	}
	for (int i = 0; i < count; i++) {
		buffer->io[i].iov_base = total ?
			(char *)buffer->data + offset : NULL;
		buffer->io[i].iov_len = buffer->user[i].iov_len;
		offset += buffer->user[i].iov_len;
	}
	if (write && fdio_buffer_copy(buffer, total, 1) != (ssize_t)total)
		goto fail;
	return 0;
fail:
	fdio_buffer_free(buffer);
	return rc;
}

static ssize_t fdio_buffer_finish(struct fdio_buffer *buffer, ssize_t result,
				 int write)
{
	if (!write && result > 0)
		result = fdio_buffer_copy(buffer, result, 0);
	fdio_buffer_free(buffer);
	return result;
}

ssize_t uk_sys_preadv(struct uk_ofile *of, const struct iovec *iov, int iovcnt,
		      off_t offset)
{
	struct fdio_buffer buffer;
	ssize_t r;
	long flags;
	int iolock;
	const struct uk_file *f;

	unsigned int mode = of->mode;

	if (unlikely(!_CAN_READ(mode)))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;
	if (unlikely(offset < 0))
		return -EINVAL;

	if (!_IS_SEEKABLE(mode))
		return -ESPIPE;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0);
	if (r)
		return r;
	iov = buffer.io;

	flags = mode & _READ_MODEMASK;
	iolock = _SHOULD_LOCK(mode);
	f = of->file;
	for (;;) {
		if (iolock)
			uk_file_rlock(f);
		r = uk_file_read(f, iov, (size_t)iovcnt, offset, flags);
		if (iolock)
			uk_file_runlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		r = fdio_wait(f, UKFD_POLLIN|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 0);
	}
	r = fdio_buffer_finish(&buffer, r, 0);

	return r;
}

ssize_t uk_sys_pread(struct uk_ofile *of, void *buf, size_t count, off_t offset)
{
	return uk_sys_preadv(of, &_buf2iov(buf, count), 1, offset);
}

ssize_t uk_sys_readv(struct uk_ofile *of, const struct iovec *iov, int iovcnt)
{
	struct fdio_buffer buffer;
	ssize_t r;
	long flags;
	size_t off;
	const struct uk_file *f;
	int seekable;
	int iolock;

	unsigned int mode = of->mode;

	if (unlikely(!_CAN_READ(mode)))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	iolock = _SHOULD_LOCK(mode);

	off = 0;
	flags = mode & _READ_MODEMASK;
	f = of->file;
	for (;;) {
		if (seekable) {
			_of_lock(of);
			off = of->pos;
		}

		if (iolock)
			uk_file_rlock(f);
		r = uk_file_read(f, iov, (size_t)iovcnt, off, flags);
		if (iolock)
			uk_file_runlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		if (seekable)
			_of_unlock(of);
		r = fdio_wait(f, UKFD_POLLIN|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 0);
	}

	r = fdio_buffer_finish(&buffer, r, 0);

	if (seekable) {
		if (r > 0)
			of->pos = off + r;
		_of_unlock(of);
	}

	return r;
}

ssize_t uk_sys_read(struct uk_ofile *of, void *buf, size_t count)
{
	return uk_sys_readv(of, &_buf2iov(buf, count), 1);
}

ssize_t uk_sys_preadv2(struct uk_ofile *of, const struct iovec *iov, int iovcnt,
		       off_t offset, int flags)
{
	struct fdio_buffer buffer;
	ssize_t r;
	size_t off;
	long xflags;
	const struct uk_file *f;
	unsigned int mode;
	int seekable;
	int use_pos;
	int iolock;

	mode = of->mode;

	if (unlikely(!_CAN_READ(mode)))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;
	if (unlikely(offset < -1))
		return -EINVAL;

	if (flags & ~(RWF_SYNC | RWF_DSYNC | RWF_APPEND))
		return -EOPNOTSUPP;
	if (offset != -1 && !_IS_SEEKABLE(mode))
		return -ESPIPE;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	use_pos = offset == -1 && seekable;
	iolock = _SHOULD_LOCK(mode);

	off = offset != -1 ? offset : 0;
	xflags = mode & _READ_MODEMASK;
	f = of->file;

	for (;;) {
		if (use_pos) {
			_of_lock(of);
			off = of->pos;
		}

		if (iolock)
			uk_file_rlock(f);
		r = uk_file_read(f, iov, (size_t)iovcnt, off, xflags);
		if (iolock)
			uk_file_runlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		if (use_pos)
			_of_unlock(of);
		r = fdio_wait(f, UKFD_POLLIN|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 0);
	}

	r = fdio_buffer_finish(&buffer, r, 0);

	if (use_pos) {
		if (r > 0)
			of->pos = off + r;
		_of_unlock(of);
	}

	return r;
}


ssize_t uk_sys_pwritev(struct uk_ofile *of, const struct iovec *iov, int iovcnt,
		       off_t offset)
{
	struct fdio_buffer buffer;
	ssize_t r;
	long flags;
	const struct uk_file *f;
	int iolock;

	unsigned int mode = of->mode;

	if (!_CAN_WRITE(mode))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;
	if (unlikely(offset < 0))
		return -EINVAL;

	if (!_IS_SEEKABLE(mode))
		return -ESPIPE;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 1);
	if (r)
		return r;
	iov = buffer.io;

	flags = mode & _WRITE_MODEMASK;
	iolock = _SHOULD_LOCK(mode);
	f = of->file;
	for (;;) {
		if (iolock)
			uk_file_wlock(f);
		off_t off = _IS_APPEND(mode) ? fdio_get_eof(f) : offset;

		r = off < 0 ? off :
			uk_file_write(f, iov, (size_t)iovcnt, off, flags);
		if (iolock)
			uk_file_wunlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		r = fdio_wait(f, UKFD_POLLOUT|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 1);
	}

	r = fdio_buffer_finish(&buffer, r, 1);

	return r;
}

ssize_t uk_sys_pwrite(struct uk_ofile *of, const void *buf, size_t count,
		  off_t offset)
{
	return uk_sys_pwritev(of, &_buf2iov((void *)buf, count), 1, offset);
}

ssize_t uk_sys_writev(struct uk_ofile *of, const struct iovec *iov, int iovcnt)
{
	struct fdio_buffer buffer;
	ssize_t r;
	off_t off;
	long flags;
	const struct uk_file *f;
	int seekable;
	int iolock;

	unsigned int mode = of->mode;

	if (!_CAN_WRITE(mode))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 1);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	iolock = _SHOULD_LOCK(mode);

	off = 0;
	flags = mode & _WRITE_MODEMASK;
	f = of->file;
	for (;;) {
		if (seekable)
			_of_lock(of);
		if (iolock)
			uk_file_wlock(f);

		if (seekable) {
			if (_IS_APPEND(mode))
				off = fdio_get_eof(f);
			else
				off = of->pos;
		}

		if (likely(off >= 0))
			r = uk_file_write(f, iov, (size_t)iovcnt, off, flags);
		else
			r = off;

		if (iolock)
			uk_file_wunlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		if (seekable)
			_of_unlock(of);
		r = fdio_wait(f, UKFD_POLLOUT|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 1);
	}

	r = fdio_buffer_finish(&buffer, r, 1);

	if (seekable) {
		if (r >= 0)
			of->pos = off + r;
		_of_unlock(of);
	}

	return r;
}

ssize_t uk_sys_write(struct uk_ofile *of, const void *buf, size_t count)
{
	return uk_sys_writev(of, &_buf2iov((void *)buf, count), 1);
}

ssize_t uk_sys_pwritev2(struct uk_ofile *of, const struct iovec *iov,
			int iovcnt, off_t offset, int flags)
{
	struct fdio_buffer buffer;
	ssize_t r;
	off_t off;
	long xflags;
	const struct uk_file *f;
	int seekable;
	int use_pos;
	int iolock;

	unsigned int mode = of->mode;

	if (!_CAN_WRITE(mode))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;
	if (unlikely(offset < -1))
		return -EINVAL;

	if (flags & ~(RWF_SYNC | RWF_DSYNC | RWF_APPEND))
		return -EOPNOTSUPP;
	if (offset != -1 && !_IS_SEEKABLE(mode))
		return -ESPIPE;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 1);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	use_pos = seekable && offset == -1;
	iolock = _SHOULD_LOCK(mode);

	off = seekable ? offset : 0;
	xflags = mode & _WRITE_MODEMASK;
	if (flags & RWF_SYNC)
		xflags |= O_SYNC;
	if (flags & RWF_DSYNC)
		xflags |= O_DSYNC;
	f = of->file;

	for (;;) {
		if (use_pos)
			_of_lock(of);
		if (iolock)
			uk_file_wlock(f);

		if (seekable) {
			if ((flags & RWF_APPEND) || _IS_APPEND(mode))
				off = fdio_get_eof(f);
			else if (use_pos)
				off = of->pos;
		}

		if (likely(off >= 0))
			r = uk_file_write(f, iov, (size_t)iovcnt, off, xflags);
		else
			r = off;

		if (iolock)
			uk_file_wunlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		if (use_pos)
			_of_unlock(of);
		r = fdio_wait(f, UKFD_POLLOUT|UKFD_POLL_ALWAYS);
		if (r)
			return fdio_buffer_finish(&buffer, r, 1);
	}

	r = fdio_buffer_finish(&buffer, r, 1);

	if (use_pos) {
		if (r >= 0) {
			UK_ASSERT(off >= 0);
			of->pos = off + r;
		}
		_of_unlock(of);
	}

	return r;
}


off_t uk_sys_lseek(struct uk_ofile *of, off_t offset, int whence)
{
	unsigned int mode;
	int iolock;

	if (unlikely(!(whence == SEEK_SET ||
		       whence == SEEK_CUR ||
		       whence == SEEK_END)))
		return -EINVAL;

	mode = of->mode;

	if (unlikely(!_IS_SEEKABLE(mode)))
		return -ESPIPE;

	iolock = _SHOULD_LOCK(mode);

	_of_lock(of);
	if (whence == SEEK_END) {
		const struct uk_file *f = of->file;
		ssize_t eof;

		if (iolock)
			uk_file_rlock(f);
		eof = fdio_get_eof(f);
		if (iolock)
			uk_file_runlock(f);
		if (eof >= 0)
			offset += eof;
		else
			offset = eof;
	} else if (whence == SEEK_CUR) {
		offset += of->pos;
	}
	if (likely(offset >= 0))
		of->pos = offset;
	else
		offset = -EINVAL;
	_of_unlock(of);

	return offset;
}

ssize_t uk_sys_sendfile(struct uk_ofile *outof __unused,
			struct uk_ofile *inof __unused,
			off_t *offset __unused, size_t count __unused)
{
	/* True sendfile requires ukfile direct memory I/O.
	 *
	 * Until then, error (truthfully) that input files are un-mmap-able.
	 */
	return -EINVAL;
}
