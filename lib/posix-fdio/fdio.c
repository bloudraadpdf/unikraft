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
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
#include <uk/posix-signal-file.h>
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif

#include "fdio-impl.h"

/* Stable mode bits to pass onto the read/write implementations */
#define _READ_MODEMASK (O_DIRECT | O_NOATIME | O_NONBLOCK)
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

struct uk_fdio_wait_context uk_fdio_wait_init(const struct uk_file *file,
					      uk_pollevent events)
{
	struct uk_fdio_wait_context context = {
	    .file = file, .events = events | UKFD_POLL_ALWAYS};
	__nsec duration;
	__nsec now;

	if (!file->ops->io_timeout)
		return context;
	duration = file->ops->io_timeout(file, events);
	if (duration == __NSEC_MAX)
		return context;
	now = ukplat_monotonic_clock();
	context.deadline =
	    duration > __SNSEC_MAX - now ? __SNSEC_MAX : now + duration;
	if (!context.deadline)
		context.deadline = 1;
	return context;
}

#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
static void fdio_signal_file_notify(uk_pollevent events,
				    enum uk_poll_chain_op operation __unused,
				    struct uk_poll_chain *ticket)
{
	uk_pollq_notify_io(ticket->arg, events);
}
#endif

static inline bool fdio_changed(struct uk_pollq *queue, unsigned long gen,
				const unsigned int *intr)
{
	return uk_pollq_io_generation(queue) != gen ||
		(intr && uk_load_n(intr));
}

int uk_fdio_wait(const struct uk_fdio_wait_context *context)
{
	struct uk_pollq *q = &context->file->state->pollq;
	const uk_pollevent events = context->events;
	const __nsec deadline = context->deadline;
	const unsigned long gen = context->generation;
	int rc = 0;
	unsigned int *intr = NULL;
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	struct uk_posix_signal_file_poll signal_poll;
	struct uk_poll_chain ticket =
	    UK_POLL_CHAIN_CALLBACK(events, fdio_signal_file_notify, q);
	uk_pollevent ready;
	bool signal_file;
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	struct uk_posix_signal_wait wait;

	rc = uk_posix_signal_wait_init(&wait, &q->waitq, NULL, 0);
	if (rc)
		return rc;
	intr = &wait.interrupted;
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	signal_file = uk_posix_signal_file_poll_register(context->file,
							 &signal_poll,
							 &ticket,
							 &ready);
	if (signal_file && ready)
		uk_pollq_notify_io(q, ready);
#endif
	for (;;) {
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
		rc = uk_posix_signal_wait_check(&wait);
		if (rc)
			break;
#endif
		if (deadline && ukplat_monotonic_clock() >= deadline) {
			rc = -EAGAIN;
			break;
		}
		uk_rwlock_rlock(&q->waitlock);
		if (fdio_changed(q, gen, NULL)) {
			uk_rwlock_runlock(&q->waitlock);
			break;
		}
		(void)uk_or(&q->waitmask, events);
		uk_waitq_set_cookie(events);
		uk_waitq_wait_deadline_unless_locked(&q->waitq,
						     fdio_changed(q, gen, intr),
						     deadline,
						     uk_rwlock_rlock,
						     uk_rwlock_runlock,
						     &q->waitlock);
		uk_rwlock_runlock(&q->waitlock);
	}
#if CONFIG_LIBPOSIX_PROCESS_SIGNALFD
	if (signal_file)
		uk_posix_signal_file_poll_unregister(&signal_poll);
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	uk_posix_signal_wait_end(&wait, rc,
				 deadline ? UK_POSIX_SIGNAL_RESTART_NONE
					  : UK_POSIX_SIGNAL_RESTART_HANDLER);
#endif
	return rc;
}


static ssize_t fdio_write_result(const struct uk_file *file __maybe_unused,
				 size_t count, int error)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	if (error == -EPIPE && file->write_sigpipe)
		(void)uk_posix_signal_current(SIGPIPE);
#endif
	return count ? (ssize_t)count : error;
}

struct fdio_buffer {
	struct iovec *user;
	struct iovec *io;
	void *data;
	size_t length;
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
			    int count, int write, bool nofault)
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
	buffer->length = total;
	if (nofault) {
		buffer->io = buffer->user;
		return 0;
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
	if (!write && buffer->io != buffer->user && result > 0)
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
	struct uk_fdio_wait_context wait;

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

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0,
			     of->file->nofault_io);
	if (r)
		return r;
	iov = buffer.io;

	flags = mode & _READ_MODEMASK;
	iolock = _SHOULD_LOCK(mode);
	f = of->file;
	wait = uk_fdio_wait_init(f, UKFD_POLLIN);
	for (;;) {
		uk_fdio_wait_snapshot(&wait);
		if (iolock)
			uk_file_rlock(f);
		r = uk_file_read(f, iov, (size_t)iovcnt, offset, flags);
		if (iolock)
			uk_file_runlock(f);
		if (!_SHOULD_BLOCK(r, mode))
			break;
		r = uk_fdio_wait(&wait);
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
	struct uk_fdio_wait_context wait;
	int seekable;
	int iolock;

	unsigned int mode = of->mode;

	if (unlikely(!_CAN_READ(mode)))
		return -EBADF;
	if (unlikely(iovcnt < 0))
		return -EINVAL;
	if (unlikely(!iov && iovcnt))
		return -EFAULT;

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0,
			     of->file->nofault_io);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	iolock = _SHOULD_LOCK(mode);

	off = 0;
	flags = mode & _READ_MODEMASK;
	f = of->file;
	wait = uk_fdio_wait_init(f, UKFD_POLLIN);
	for (;;) {
		uk_fdio_wait_snapshot(&wait);
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
		r = uk_fdio_wait(&wait);
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
	struct uk_fdio_wait_context wait;
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

	r = fdio_buffer_init(&buffer, iov, iovcnt, 0,
			     of->file->nofault_io);
	if (r)
		return r;
	iov = buffer.io;

	seekable = _IS_SEEKABLE(mode);
	use_pos = offset == -1 && seekable;
	iolock = _SHOULD_LOCK(mode);

	off = offset != -1 ? offset : 0;
	xflags = mode & _READ_MODEMASK;
	f = of->file;
	wait = uk_fdio_wait_init(f, UKFD_POLLIN);

	for (;;) {
		uk_fdio_wait_snapshot(&wait);
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
		r = uk_fdio_wait(&wait);
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


static int fdio_iov_advance(struct iovec **current, size_t *count, size_t bytes)
{
	struct iovec *iov = *current;

	while (*count && bytes >= iov->iov_len) {
		bytes -= iov->iov_len;
		iov++;
		(*count)--;
	}
	if (bytes) {
		__uptr base = (__uptr)iov->iov_base;

		if (base + bytes < base)
			return -EFAULT;
		iov->iov_base = (void *)(base + bytes);
		iov->iov_len -= bytes;
	}
	*current = iov;
	return 0;
}

static ssize_t fdio_writev(struct uk_ofile *of, const struct iovec *iov,
			   int iovcnt, off_t offset, int flags, bool relative,
			   enum uk_file_write_kind kind)
{
	struct fdio_buffer buffer;
	const struct uk_file *file = of->file;
	struct uk_fdio_wait_context wait;
	struct iovec *current;
	const unsigned int mode = of->mode;
	const bool seekable = _IS_SEEKABLE(mode);
	const bool use_pos = seekable && offset == -1;
	const bool iolock = _SHOULD_LOCK(mode);
	size_t count, total = 0;
	long xflags = mode & _WRITE_MODEMASK;
	int error;

	if (mode & O_PATH)
		return -EBADF;
	if (offset < (relative ? -1 : 0))
		return -EINVAL;
	if (offset != -1 && (!seekable || file->no_pwrite))
		return -ESPIPE;
	if (!fdio_can_write(mode))
		return -EBADF;
	if (iovcnt < 0)
		return -EINVAL;
	if (!iov && iovcnt)
		return -EFAULT;
	if (flags & ~(RWF_SYNC | RWF_DSYNC | RWF_APPEND))
		return -EOPNOTSUPP;

	error = fdio_buffer_init(&buffer, iov, iovcnt, 1, file->nofault_io);
	if (error)
		return error;
	current = buffer.io;
	count = buffer.count;
	if (flags & RWF_SYNC)
		xflags |= O_SYNC;
	if (flags & RWF_DSYNC)
		xflags |= O_DSYNC;
	wait = uk_fdio_wait_init(file, UKFD_POLLOUT);
	for (;;) {
		struct uk_file_io_result result;
		off_t off = 0;

		uk_fdio_wait_snapshot(&wait);
		if (use_pos)
			_of_lock(of);
		if (iolock)
			uk_file_wlock(file);
		if (seekable) {
			if ((flags & RWF_APPEND) || _IS_APPEND(mode))
				off = fdio_get_eof(file);
			else if (use_pos)
				off = of->pos;
			else if (total > __SSZ_MAX - (size_t)offset)
				off = -EFBIG;
			else
				off = offset + total;
		}
		result = off < 0 ? uk_file_io_status(off)
				 : uk_file_write(file, current, count, off,
						 xflags, kind);
		UK_ASSERT(result.count <= buffer.length - total);
		UK_ASSERT(result.error <= 0);
		if (iolock)
			uk_file_wunlock(file);
		if (use_pos) {
			if (result.count &&
			    result.position == UK_FILE_WRITE_ADVANCE) {
				UK_ASSERT(off >= 0 &&
					  result.count <=
					  __SSZ_MAX - (size_t)off);
				of->pos = off + result.count;
			}
			_of_unlock(of);
		}
		total += result.count;
		error = result.error;
		if (error != -EAGAIN || !_IS_BLOCKING(mode) ||
		    (result.count && total == buffer.length))
			break;
		error = fdio_iov_advance(&current, &count, result.count);
		if (error)
			break;
		error = uk_fdio_wait(&wait);
		if (error)
			break;
	}
	fdio_buffer_free(&buffer);
	return fdio_write_result(file, total, error);
}

ssize_t uk_sys_pwritev(struct uk_ofile *of, const struct iovec *iov, int iovcnt,
		       off_t offset)
{
	return fdio_writev(of, iov, iovcnt, offset, 0, false,
			   UK_FILE_WRITE_VECTOR);
}

ssize_t uk_sys_pwrite(struct uk_ofile *of, const void *buf, size_t count,
		      off_t offset)
{
	return fdio_writev(of, &_buf2iov((void *)buf, count), 1, offset, 0,
			   false, UK_FILE_WRITE_SCALAR);
}

ssize_t uk_sys_writev(struct uk_ofile *of, const struct iovec *iov, int iovcnt)
{
	return fdio_writev(of, iov, iovcnt, -1, 0, true, UK_FILE_WRITE_VECTOR);
}

ssize_t uk_sys_write(struct uk_ofile *of, const void *buf, size_t count)
{
	return fdio_writev(of, &_buf2iov((void *)buf, count), 1, -1, 0, true,
			   UK_FILE_WRITE_SCALAR);
}

ssize_t uk_sys_pwritev2(struct uk_ofile *of, const struct iovec *iov,
			int iovcnt, off_t offset, int flags)
{
	return fdio_writev(of, iov, iovcnt, offset, flags, true,
			   UK_FILE_WRITE_VECTOR);
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
	if (mode & O_PATH)
		return -EBADF;

	if (unlikely(!_IS_SEEKABLE(mode)))
		return -ESPIPE;

	iolock = _SHOULD_LOCK(mode);

	_of_lock(of);
	if (of->file->ops->seek) {
		offset = of->file->ops->seek(of->file, offset, whence, of->pos);
		if (offset >= 0)
			of->pos = offset;
		_of_unlock(of);
		return offset;
	}
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
