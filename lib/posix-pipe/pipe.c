/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <uk/atomic.h>
#include <uk/alloc.h>
#include <uk/essentials.h>
#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/file/nops.h>
#include <uk/file/iovutil.h>
#include <uk/posix-fd.h>
#include <uk/posix-pipe.h>

#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#include <uk/syscall.h>
#endif /* CONFIG_LIBPOSIX_FDTAB */

#define PIPE_SIZE (1L << CONFIG_LIBPOSIX_PIPE_SIZE_ORDER)

#if CONFIG_LIBPOSIX_PIPE_PACKET
#define PIPE_MSGCOUNT CONFIG_LIBPOSIX_PIPE_MAX_PACKETS
#else /* !CONFIG_LIBPOSIX_PIPE_PACKET */
#define PIPE_MSGCOUNT 1
#endif

#define _PIPE_SZMASK (PIPE_SIZE - 1)
#define PIPE_IDX(x) ((x) & _PIPE_SZMASK)

#define PIPE_SPACE(start, lim) \
	(((start) <= (lim)) ? (size_t)((lim) - (start)) : \
			      (size_t)(PIPE_SIZE - (start) + (lim)))
#define PIPE_USED(start, lim) \
	(((start) == (lim)) ? PIPE_SIZE : PIPE_SPACE(start, lim))

static const char PIPE_VOLID[] = "pipe_vol";

#define PIPE_R_FNAME "pipe:read"
#define PIPE_R_FNAME_LEN (sizeof(PIPE_R_FNAME) - 1)

#define PIPE_W_FNAME "pipe:write"
#define PIPE_W_FNAME_LEN (sizeof(PIPE_W_FNAME) - 1)

typedef __u32 pipeidx;

struct pipe_msg {
	struct pipe_msg *next;
	pipeidx start;
	pipeidx end;
};

struct pipe_node {
	struct pipe_msg *volatile head;
	struct pipe_msg *tail;
	struct pipe_msg *free;
	unsigned int readers;
	unsigned int writers;
	size_t limit;
	size_t used;
	struct pipe_msg msgs[PIPE_MSGCOUNT];
	char buf[PIPE_SIZE];
};

struct pipe_alloc {
	struct uk_alloc *alloc;
	struct uk_mutex lock;
	struct uk_list_head ends;
	unsigned int references;
	struct pipe_node node;
};

struct pipe_end {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_list_head link;
	unsigned int mode;
};

static void pipe_events(struct pipe_alloc *pipe)
{
	struct pipe_node *node = &pipe->node;
	struct pipe_end *end;
	bool writable = node->used < node->limit &&
		(!node->head ||
		 (PIPE_SPACE(node->tail->end, node->head->start) &&
		  (node->tail->next == node->tail || node->free)));

	uk_list_for_each_entry(end, &pipe->ends, link) {
		uk_pollevent events = 0;

		if (!(end->mode & (O_PATH | O_WRONLY))) {
			if (node->head)
				events |= UKFD_POLLIN;
			if (!node->writers)
				events |= EPOLLHUP;
		}
		if (!(end->mode & O_PATH) &&
		    (end->mode & (O_WRONLY | O_RDWR))) {
			if (writable)
				events |= UKFD_POLLOUT;
			if (!node->readers)
				events |= EPOLLERR;
		}
		uk_file_event_assign(&end->file, events);
	}
}

static void pipebuf_iovread(const char *buf, pipeidx head, size_t toread,
			    const struct iovec *iov, size_t iovcnt)
{
	size_t iovi = 0;
	size_t cur = 0;
	size_t canread = MIN(toread, (size_t)(PIPE_SIZE - head));
	size_t read;

	read = uk_iov_scatter(iov, iovcnt, &buf[head], canread, &iovi, &cur);
	toread -= read;
	if (toread) {
		head = PIPE_IDX(head + read);
		(void)uk_iov_scatter(iov, iovcnt, &buf[head], toread,
				     &iovi, &cur);
	}
}

static void pipebuf_iovwrite(char *buf, pipeidx head, size_t towrite,
			     const struct iovec *iov, size_t iovcnt)
{
	size_t iovi = 0;
	size_t cur = 0;
	size_t canwrite = MIN(towrite, (size_t)(PIPE_SIZE - head));
	size_t written;

	written = uk_iov_gather(&buf[head], iov, iovcnt, canwrite, &iovi, &cur);
	towrite -= written;
	if (towrite) {
		head = PIPE_IDX(head + written);
		(void)uk_iov_gather(&buf[head], iov, iovcnt, towrite,
				    &iovi, &cur);
	}
}

static ssize_t pipe_read(const struct uk_file *f,
			 const struct iovec *iov, size_t iovcnt,
			 size_t off, long flags)
{
	size_t toread;
	struct pipe_node *d;
	struct pipe_msg *m;
	struct pipe_msg *prev;
	size_t canread;
	size_t consumed;
	pipeidx start;

	UK_ASSERT(f->vol == PIPE_VOLID);
	if (unlikely(off))
		return -ESPIPE;
	if (unlikely(!iov))
		return -EFAULT;

	toread = uk_iov_len(iov, iovcnt);
	if (unlikely(!toread))
		return 0;

	d = (struct pipe_node *)f->node;
	m = d->head;
	for (;;) {
		if (!m) {
			if (!d->writers)
				return 0;
			else
				return -EAGAIN;
		}

		if (flags & UK_PIPE_READ_PEEK) {
			canread = MIN(PIPE_USED(m->start, m->end), toread);
			pipebuf_iovread(d->buf, m->start, canread, iov, iovcnt);
			return canread;
		}

		if (m->next != m) { /* Packet msg */
			if (!uk_compare_exchange_n(&d->head, &m, m->next))
				continue;
			if (!m->next) {
				d->tail = NULL;
			}

			start = m->start;
			consumed = PIPE_USED(start, m->end);
			canread = MIN(consumed, toread);
			UK_ASSERT(canread); /* 0-length packets not allowed */
			prev = uk_exchange_n(&d->free, m);
			m->next = prev;
		} else { /* Stream msg; always last in queue */
			size_t avail;

			start = m->start;
			if (start == PIPE_SIZE) {
				m = d->head;
				continue;
			}
			avail = PIPE_USED(start, m->end);
			canread = MIN(avail, toread);
			UK_ASSERT(canread);

			if (canread == avail) {
				if (!uk_compare_exchange_n(&d->head, &m, NULL))
					continue;
				while (!uk_compare_exchange_n(&m->start,
							      &start,
							      PIPE_SIZE));

				canread = MIN(PIPE_USED(start, m->end), toread);

				d->tail = NULL;
				prev = uk_exchange_n(&d->free, m);
				m->next = prev;
			} else {
				pipeidx lim = PIPE_IDX(start + canread);

				UK_ASSERT(lim != start);
				if (!uk_compare_exchange_n(&m->start,
							   &start, lim)) {
					m = d->head;
					continue;
				}
			}
			consumed = canread;
		}
		break;
	}
	UK_ASSERT(canread);
	pipebuf_iovread(d->buf, start, canread, iov, iovcnt);
	UK_ASSERT(d->used >= consumed);
	d->used -= consumed;

	return canread;
}

static ssize_t pipe_write(const struct uk_file *f,
			  const struct iovec *iov, size_t iovcnt,
			  size_t off, long flags)
{
	struct pipe_node *d;
	struct pipe_msg *tail;
	size_t towrite;
	size_t canwrite;
	size_t capacity;
	size_t available;
	pipeidx whead;
	pipeidx wend;

	UK_ASSERT(f->vol == PIPE_VOLID);
	if (unlikely(off))
		return -ESPIPE;
	if (unlikely(!iov))
		return -EFAULT;

	towrite = uk_iov_len(iov, iovcnt);
	if (unlikely(!towrite))
		return 0;

	d = (struct pipe_node *)f->node;
	if (unlikely(!d->readers))
		return -EPIPE;

	available = d->used < d->limit ? d->limit - d->used : 0;
	tail = d->tail;
	if (!tail || tail->next != tail) { /* Empty or last msg is packet */
		struct pipe_msg *m;

		if (tail) {
			UK_ASSERT(d->head);
			UK_ASSERT(!tail->next);
			whead = tail->end;
			wend = d->head->start;
		} else {
			UK_ASSERT(!d->head);
			whead = 0;
			wend = PIPE_SIZE;
		}
		capacity = MIN(PIPE_SPACE(whead, wend), available);
		if ((flags & UK_PIPE_WRITE_ATOMIC) && towrite > capacity)
			return -EAGAIN;
		canwrite = MIN(capacity, towrite);
		if (!canwrite)
			goto out_full;
		wend = PIPE_IDX(whead + canwrite);

		m = d->free;
		if (!m)
			goto out_full;
		d->free = m->next;

		m->start = whead;
		m->end = wend;
		m->next = (flags & O_DIRECT) ? NULL : m;

		d->tail = m;
		if (tail)
			tail->next = m;
		else
			d->head = m;
	} else { /* Last msg is stream, extend */
		UK_ASSERT(d->head);
		whead = tail->end;
		wend = d->head->start;
		capacity = MIN(PIPE_SPACE(whead, wend), available);
		if ((flags & UK_PIPE_WRITE_ATOMIC) && towrite > capacity)
			return -EAGAIN;
		canwrite = MIN(towrite, capacity);
		if (!canwrite)
			goto out_full;
		tail->end = PIPE_IDX(whead + canwrite);
	}
	UK_ASSERT(canwrite);
	pipebuf_iovwrite(d->buf, whead, canwrite, iov, iovcnt);
	d->used += canwrite;
	return canwrite;

out_full:

	return -EAGAIN;
}

static int pipe_getstat(const struct uk_file *f, unsigned mask __unused,
			struct uk_statx *arg)
{
	/* All data is immediately available, ignore mask */
	memset(arg, 0, sizeof(*arg));
	arg->stx_mask = UK_STATX_TYPE | UK_STATX_MODE | UK_STATX_INO;
	arg->stx_blksize = 1;
	arg->stx_mode = S_IFIFO | 0600;
	arg->stx_ino = (uintptr_t)f->node;
	return 0;
}

static struct pipe_alloc *pipe_from_file(const struct uk_file *file)
{
	return __containerof((struct pipe_node *)file->node,
			     struct pipe_alloc, node);
}

int uk_pipefile_set_capacity(const struct uk_file *file, size_t capacity)
{
	struct pipe_alloc *pipe;

	if (file->vol != PIPE_VOLID || !capacity || capacity > PIPE_SIZE)
		return -EINVAL;
	pipe = pipe_from_file(file);
	uk_mutex_lock(&pipe->lock);
	pipe->node.limit = capacity;
	pipe_events(pipe);
	uk_mutex_unlock(&pipe->lock);
	return 0;
}

static ssize_t pipe_io(const struct uk_file *file,
		       const struct iovec *iov, size_t count, size_t off,
		       long flags, bool write)
{
	struct pipe_alloc *pipe = pipe_from_file(file);
	const struct pipe_end *end = __containerof(file, struct pipe_end, file);
	ssize_t result;

	if ((end->mode & O_PATH) ||
	    (write ? !(end->mode & (O_WRONLY | O_RDWR)) :
		     !!(end->mode & O_WRONLY)))
		return -EBADF;
	uk_mutex_lock(&pipe->lock);
	result = write ? pipe_write(file, iov, count, off, flags) :
			 pipe_read(file, iov, count, off, flags);
	pipe_events(pipe);
	uk_mutex_unlock(&pipe->lock);
	return result;
}

static ssize_t pipe_read_end(const struct uk_file *file,
			     const struct iovec *iov, size_t count,
			     size_t off, long flags)
{
	return pipe_io(file, iov, count, off, flags, false);
}

static ssize_t pipe_write_end(const struct uk_file *file,
			      const struct iovec *iov, size_t count,
			      size_t off, long flags)
{
	return pipe_io(file, iov, count, off, flags, true);
}

static const struct uk_file *pipe_reopen(const struct uk_file *file,
					 unsigned int mode);

static const struct uk_file_ops pipe_ops = {
	.read = pipe_read_end,
	.write = pipe_write_end,
	.mem = uk_file_nop_mem,
	.getstat = pipe_getstat,
	.setstat = uk_file_nop_setstat,
	.ctl = uk_file_nop_ctl,
	.reopen = pipe_reopen
};

static void pipe_release(const struct uk_file *file, int what)
{
	struct pipe_alloc *pipe = pipe_from_file(file);
	struct uk_alloc *allocator = pipe->alloc;
	struct pipe_end *end = __containerof(file, struct pipe_end, file);
	bool last = false;

	uk_mutex_lock(&pipe->lock);
	if (what & UK_FILE_RELEASE_RES) {
		uk_list_del(&end->link);
		if (!(end->mode & O_PATH)) {
			if (!(end->mode & O_WRONLY))
				pipe->node.readers--;
			if (end->mode & (O_WRONLY | O_RDWR))
				pipe->node.writers--;
		}
		pipe_events(pipe);
	}
	if (what & UK_FILE_RELEASE_OBJ)
		last = !--pipe->references;
	uk_mutex_unlock(&pipe->lock);
	if (what & UK_FILE_RELEASE_OBJ)
		uk_free(allocator, end);
	if (last)
		uk_free(allocator, pipe);
}

static struct pipe_end *pipe_open(struct pipe_alloc *pipe, unsigned int mode)
{
	struct pipe_end *end = uk_malloc(pipe->alloc, sizeof(*end));

	if (!end)
		return NULL;
	end->references = UK_FILE_REFCNT_INIT_VALUE(end->references);
	end->state = UK_FILE_STATE_INIT_VALUE(end->state);
	end->mode = mode;
	end->file = (struct uk_file){
		.vol = PIPE_VOLID,
		.node = &pipe->node,
		.refcnt = &end->references,
		.state = &end->state,
		.ops = &pipe_ops,
		._release = pipe_release
	};
	uk_mutex_lock(&pipe->lock);
	pipe->references++;
	if (!(mode & O_PATH)) {
		if (!(mode & O_WRONLY))
			pipe->node.readers++;
		if (mode & (O_WRONLY | O_RDWR))
			pipe->node.writers++;
	}
	uk_list_add_tail(&end->link, &pipe->ends);
	pipe_events(pipe);
	uk_mutex_unlock(&pipe->lock);
	return end;
}

static const struct uk_file *pipe_reopen(const struct uk_file *file,
					 unsigned int mode)
{
	struct pipe_end *end = pipe_open(pipe_from_file(file), mode);

	return end ? &end->file : ERR2PTR(-ENOMEM);
}

int uk_pipefile_create(struct uk_file *pipes[2])
{
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct pipe_alloc *pipe = uk_zalloc(allocator, sizeof(*pipe));
	struct pipe_end *reader, *writer;

	if (!pipe)
		return -ENOMEM;
	pipe->alloc = allocator;
	pipe->node.limit = PIPE_SIZE;
	uk_mutex_init(&pipe->lock);
	UK_INIT_LIST_HEAD(&pipe->ends);
	pipe->node.free = &pipe->node.msgs[0];
	for (int i = 0; i < PIPE_MSGCOUNT - 1; i++)
		pipe->node.msgs[i].next = &pipe->node.msgs[i + 1];
	reader = pipe_open(pipe, O_RDONLY);
	if (!reader) {
		uk_free(allocator, pipe);
		return -ENOMEM;
	}
	writer = pipe_open(pipe, O_WRONLY);
	if (!writer) {
		uk_file_release(&reader->file);
		return -ENOMEM;
	}
	pipes[0] = &reader->file;
	pipes[1] = &writer->file;
	return 0;
}

#if CONFIG_LIBPOSIX_FDTAB
/* Internal syscalls */

#define _OPEN_FLAGS (O_CLOEXEC|O_NONBLOCK|O_DIRECT)

int uk_sys_pipe(int pipefd[2], int flags)
{
	int r;
	struct uk_file *pipes[2];
	int oflags;
	int rpipe;

#if !CONFIG_LIBPOSIX_PIPE_PACKET
	if (unlikely(flags & O_DIRECT)) {
		uk_pr_warn_once("packet pipes requested but not enabled\n");
		return -EINVAL;
	}
#endif /* CONFIG_LIBPOSIX_PIPE_PACKET */

	r = uk_pipefile_create(pipes);
	if (unlikely(r))
		return r;

	oflags = (flags & _OPEN_FLAGS) | UKFD_O_NOSEEK;
	r = uk_fdtab_open_named(pipes[0], O_RDONLY | oflags,
				PIPE_R_FNAME, PIPE_R_FNAME_LEN);
	if (unlikely(r < 0))
		goto err_free;

	rpipe = r;

	r = uk_fdtab_open_named(pipes[1], O_WRONLY | oflags,
				PIPE_W_FNAME, PIPE_W_FNAME_LEN);
	if (unlikely(r < 0))
		goto err_close;

	uk_file_release(pipes[0]);
	uk_file_release(pipes[1]);
	pipefd[0] = rpipe;
	pipefd[1] = r;
	return  0;

err_close:
	uk_sys_close(rpipe);
err_free:
	uk_file_release(pipes[0]);
	uk_file_release(pipes[1]);
	return r;
}

/* Syscalls */

UK_SYSCALL_R_DEFINE(int, pipe, int *, pipefd)
{
	return uk_sys_pipe(pipefd, 0);
}

UK_SYSCALL_R_DEFINE(int, pipe2, int *, pipefd, int, flags)
{
	return uk_sys_pipe(pipefd, flags);
}
#endif /* CONFIG_LIBPOSIX_FDTAB */
