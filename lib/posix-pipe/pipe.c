/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <uk/atomic.h>
#include <uk/arch/limits.h>
#include <uk/alloc.h>
#include <uk/essentials.h>
#include <uk/errptr.h>
#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/file/nops.h>
#include <uk/file/iovutil.h>
#include <uk/posix-fd.h>
#include <uk/posix-pipe.h>
#include <uk/posix-fdio.h>
#include <uk/nofault.h>

#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#include <uk/syscall.h>
#endif /* CONFIG_LIBPOSIX_FDTAB */

#define PIPE_SIZE (1L << CONFIG_LIBPOSIX_PIPE_SIZE_ORDER)
#define PIPE_PAGE_COUNT (PIPE_SIZE / __PAGE_SIZE)

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

struct pipe_budget {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_mutex lock;
	struct uk_alloc *allocator;
	size_t capacity;
	size_t used;
};

static const char PIPE_BUDGET_VOLID[] = "pipe_budget";

static void pipe_budget_events(struct pipe_budget *budget)
{
	if (budget->used <= budget->capacity / 4)
		uk_file_event_set(&budget->file, UKFD_POLLOUT);
	else
		uk_file_event_clear(&budget->file, UKFD_POLLOUT);
}

static void pipe_budget_release(const struct uk_file *file, int what)
{
	struct pipe_budget *budget = file->node;

	if (what & UK_FILE_RELEASE_OBJ)
		uk_free(budget->allocator, budget);
}

struct uk_file *uk_pipefile_budget_create(size_t capacity)
{
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct pipe_budget *budget = uk_zalloc(allocator, sizeof(*budget));

	if (!budget)
		return NULL;
	budget->references = UK_FILE_REFCNT_INIT_VALUE(budget->references);
	budget->state = UK_FILE_STATE_INIT_VALUE(budget->state);
	uk_mutex_init(&budget->lock);
	budget->allocator = allocator;
	budget->capacity = capacity;
	budget->file = (struct uk_file){
		.vol = PIPE_BUDGET_VOLID,
		.node = budget,
		.refcnt = &budget->references,
		.state = &budget->state,
		.ops = &uk_file_nops,
		._release = pipe_budget_release
	};
	pipe_budget_events(budget);
	return &budget->file;
}

int uk_pipefile_budget_set(const struct uk_file *file, size_t capacity)
{
	struct pipe_budget *budget = file->node;

	if (file->vol != PIPE_BUDGET_VOLID || capacity < 32 ||
	    capacity > PIPE_SIZE)
		return -EINVAL;
	uk_mutex_lock(&budget->lock);
	if (budget->capacity != capacity) {
		budget->capacity = capacity;
		pipe_budget_events(budget);
		uk_pollq_notify_io(&budget->state.pollq, UKFD_POLLOUT);
	}
	uk_mutex_unlock(&budget->lock);
	return 0;
}

static void pipe_budget_return(const struct uk_file *file, size_t charge)
{
	struct pipe_budget *budget = file->node;

	uk_mutex_lock(&budget->lock);
	UK_ASSERT(budget->used >= charge);
	budget->used -= charge;
	pipe_budget_events(budget);
	uk_pollq_notify_io(&budget->state.pollq, UKFD_POLLOUT);
	uk_mutex_unlock(&budget->lock);
	uk_file_release(file);
}

struct pipe_msg {
	struct pipe_msg *next;
	const struct uk_file *budget;
	size_t charge;
	size_t length;
	pipeidx start;
	pipeidx end;
};

struct pipe_page {
	size_t offset;
	size_t length;
	bool packet;
};

struct pipe_pages {
	size_t first;
	size_t count;
	struct pipe_page buffers[PIPE_PAGE_COUNT];
};

struct pipe_node {
	struct pipe_pages pages;
	struct pipe_msg *volatile head;
	struct pipe_msg *tail;
	struct pipe_msg *free;
	unsigned int readers;
	unsigned int writers;
	bool read_shutdown;
	size_t limit;
	size_t used;
	struct pipe_msg msgs[PIPE_MSGCOUNT];
	char buf[PIPE_SIZE];
};

struct uk_pipefile {
	enum uk_pipefile_kind kind;
	struct uk_alloc *alloc;
	struct uk_mutex lock;
	struct uk_list_head ends;
	unsigned int references;
	unsigned long readers_arrived;
	unsigned long writers_arrived;
	struct pipe_node node;
};

struct pipe_end {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_list_head link;
	unsigned int mode;
	unsigned long writers_seen;
	unsigned long peer_arrival;
	bool peer_present;
	bool named;
};

static void pipe_events(struct uk_pipefile *pipe, uk_pollevent changed)
{
	struct pipe_node *node = &pipe->node;
	struct pipe_end *end;
	bool readable = pipe->kind == UK_PIPEFILE_POSIX ?
		node->pages.count != 0 : !!node->head;
	bool writable = pipe->kind == UK_PIPEFILE_POSIX ?
		node->pages.count < node->limit / __PAGE_SIZE :
		node->used < node->limit &&
		(!node->head ||
		 ((!node->used ||
		   PIPE_SPACE(node->tail->end, node->head->start)) &&
		  (node->tail->next == node->tail || node->free)));

	uk_list_for_each_entry(end, &pipe->ends, link) {
		uk_pollevent events = 0;

		if (!(end->mode & (O_PATH | O_WRONLY))) {
			if (readable || node->read_shutdown)
				events |= UKFD_POLLIN;
			if ((node->read_shutdown || !node->writers) &&
			    (!end->named ||
			     end->writers_seen != pipe->writers_arrived))
				events |= EPOLLHUP;
		}
		if (!(end->mode & O_PATH) &&
		    (end->mode & (O_WRONLY | O_RDWR))) {
			if (writable)
				events |= UKFD_POLLOUT;
			if (!node->readers || node->read_shutdown)
				events |= EPOLLERR;
		}
		uk_file_event_assign(&end->file, events);
		uk_pollq_notify_io(&end->file.state->pollq, changed);
	}
}

static ssize_t pipebuf_iovcopy(char *buffer, pipeidx head, size_t length,
			       const struct iovec *iov, size_t count,
			       size_t offset, bool write)
{
	size_t done = 0;

	for (size_t index = 0; index < count && done < length; index++) {
		size_t position;

		if (offset >= iov[index].iov_len) {
			offset -= iov[index].iov_len;
			continue;
		}
		position = offset;
		offset = 0;
		while (position < iov[index].iov_len && done < length) {
			__uptr address = (__uptr)iov[index].iov_base + position;
			size_t bytes = MIN(iov[index].iov_len - position,
					   length - done);
			size_t copied;
			char *user = (char *)address;
			char *kernel = buffer + head;

			if (address < (__uptr)iov[index].iov_base)
				return done ? (ssize_t)done : -EFAULT;
			bytes = MIN(bytes, (size_t)(PIPE_SIZE - head));
			copied = uk_nofault_memcpy(write ? kernel : user,
						   write ? user : kernel,
						   bytes, 0);
			done += copied;
			if (copied != bytes)
				return done ? (ssize_t)done : -EFAULT;
			position += copied;
			head = PIPE_IDX(head + copied);
		}
	}
	return done;
}

static ssize_t pipe_page_read(struct pipe_node *node, const struct iovec *iov,
			      size_t count, size_t length)
{
	struct pipe_pages *pages = &node->pages;
	size_t done = 0;

	if (!pages->count)
		return !node->writers || node->read_shutdown ? 0 : -EAGAIN;
	while (pages->count && done < length) {
		struct pipe_page *page = &pages->buffers[pages->first];
		const bool packet = page->packet;
		size_t bytes = MIN(length - done, page->length);
		size_t consumed = packet ? page->length : bytes;
		pipeidx start = pages->first * __PAGE_SIZE + page->offset;
		ssize_t copied = pipebuf_iovcopy(node->buf, start, bytes,
						 iov, count, done, false);

		if (copied != (ssize_t)bytes)
			return done ? (ssize_t)done : -EFAULT;
		page->offset += consumed;
		page->length -= consumed;
		node->used -= consumed;
		done += bytes;
		if (!page->length) {
			pages->first = (pages->first + 1) % PIPE_PAGE_COUNT;
			pages->count--;
		}
		if (packet)
			break;
	}
	return done;
}

static struct uk_file_io_result pipe_page_write(struct pipe_node *node,
						const struct iovec *iov,
						size_t count, size_t length,
						long flags)
{
	struct pipe_pages *pages = &node->pages;
	size_t bytes = length % __PAGE_SIZE;
	size_t done = 0;

	if (node->read_shutdown || !node->readers)
		return uk_file_io_status(-EPIPE);
	if (pages->count && bytes) {
		size_t last =
		    (pages->first + pages->count - 1) % PIPE_PAGE_COUNT;
		struct pipe_page *tail = &pages->buffers[last];
		size_t end = tail->offset + tail->length;

		if (!tail->packet && end + bytes <= __PAGE_SIZE) {
			pipeidx start = last * __PAGE_SIZE + end;
			ssize_t copied =
				pipebuf_iovcopy(node->buf, start, bytes,
						iov, count, 0, true);

			if (copied != (ssize_t)bytes)
				return uk_file_io_status(-EFAULT);
			tail->length += bytes;
			node->used += bytes;
			done = bytes;
		}
	}
	while (done < length && pages->count < node->limit / __PAGE_SIZE) {
		size_t index = (pages->first + pages->count) % PIPE_PAGE_COUNT;
		struct pipe_page *page = &pages->buffers[index];
		pipeidx start = index * __PAGE_SIZE;
		ssize_t copied;

		bytes = MIN(length - done, (size_t)__PAGE_SIZE);
		copied = pipebuf_iovcopy(node->buf, start, bytes,
					 iov, count, done, true);
		if (copied != (ssize_t)bytes)
			return (struct uk_file_io_result){.count = done,
							  .error = -EFAULT};
		*page = (struct pipe_page){.length = bytes,
					   .packet = !!(flags & O_DIRECT)};
		pages->count++;
		node->used += bytes;
		done += bytes;
	}
	return (struct uk_file_io_result){
		.count = done, .error = done < length ? -EAGAIN : 0
	};
}

static struct uk_file_io_result pipe_page_io(struct pipe_node *node,
					     const struct iovec *iov,
					     size_t count, size_t off,
					     long flags, bool write)
{
	size_t length;

	if (off)
		return uk_file_io_status(-ESPIPE);
	if (!count)
		return uk_file_io_status(0);
	if (!iov)
		return uk_file_io_status(-EFAULT);
	length = uk_iov_len(iov, count);
	if (!length)
		return uk_file_io_status(0);
	return write ? pipe_page_write(node, iov, count, length, flags) :
		uk_file_io_status(pipe_page_read(node, iov, count, length));
}

static ssize_t pipe_socket_read(const struct uk_file *f,
				const struct iovec *iov, size_t iovcnt,
				size_t off, long flags,
				size_t *packet_size)
{
	struct pipe_node *d = (struct pipe_node *)f->node;
	struct pipe_msg *message = d->head;
	const struct uk_file *budget;
	size_t length, available, consumed, charge;
	ssize_t copied, result;
	bool packet;

	UK_ASSERT(f->vol == PIPE_VOLID);
	if (off)
		return -ESPIPE;
	if (!iov)
		return -EFAULT;
	length = uk_iov_len(iov, iovcnt);
	if (!length && (!message || message->next == message))
		return 0;
	if (!message)
		return !d->writers || d->read_shutdown ? 0 : -EAGAIN;
	packet = message->next != message;
	available = packet ? message->length :
		PIPE_USED(message->start, message->end);
	if (packet_size)
		*packet_size = packet ? available : 0;
	length = MIN(length, available);
	copied = pipebuf_iovcopy(d->buf, message->start, length,
				 iov, iovcnt, 0, false);
	result = copied != (ssize_t)length ?
		-EFAULT : copied;
	if (flags & UK_PIPE_READ_PEEK)
		return result;
	if (!packet && result <= 0)
		return result;
	consumed = packet ? available : (size_t)copied;
	budget = packet ? message->budget : NULL;
	charge = message->charge;
	if (packet || consumed == available) {
		d->head = packet ? message->next : NULL;
		if (!d->head)
			d->tail = NULL;
		message->budget = NULL;
		message->next = d->free;
		d->free = message;
	} else {
		message->start = PIPE_IDX(message->start + consumed);
	}
	UK_ASSERT(d->used >= consumed);
	d->used -= consumed;
	if (budget)
		pipe_budget_return(budget, charge);
	return result;
}

static ssize_t pipe_socket_write(const struct uk_file *f,
				 const struct iovec *iov, size_t iovcnt,
				 size_t off, long flags)
{
	struct pipe_node *d = (struct pipe_node *)f->node;
	struct pipe_msg *tail = d->tail, *message;
	size_t length, available, capacity, bytes;
	pipeidx head, end;
	ssize_t copied;
	bool extend;

	UK_ASSERT(f->vol == PIPE_VOLID);
	if (off)
		return -ESPIPE;
	if (!iov)
		return -EFAULT;
	if (d->read_shutdown)
		return -EPIPE;
	length = uk_iov_len(iov, iovcnt);
	if (!length && !(flags & UK_PIPE_WRITE_ATOMIC))
		return 0;
	if (!d->readers)
		return -EPIPE;
	available = d->used < d->limit ? d->limit - d->used : 0;
	extend = tail && tail->next == tail;
	if (!extend && !d->free)
		return -EAGAIN;
	head = tail ? tail->end : 0;
	end = d->head ? d->head->start : PIPE_SIZE;
	capacity = MIN(d->used ? PIPE_SPACE(head, end) : PIPE_SIZE, available);
	if ((flags & UK_PIPE_WRITE_ATOMIC) && length > capacity)
		return -EAGAIN;
	bytes = MIN(length, capacity);
	if (!bytes && length)
		return -EAGAIN;
	copied = pipebuf_iovcopy(d->buf, head, bytes, iov, iovcnt, 0, true);
	if (copied != (ssize_t)bytes)
		return -EFAULT;
	end = PIPE_IDX(head + bytes);
	if (extend) {
		tail->end = end;
	} else {
		message = d->free;
		d->free = message->next;
		message->budget = NULL;
		message->length = bytes;
		message->start = head;
		message->end = end;
		message->next = (flags & O_DIRECT) ? NULL : message;
		d->tail = message;
		if (tail)
			tail->next = message;
		else
			d->head = message;
	}
	d->used += bytes;
	return bytes;
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

static struct uk_pipefile *pipe_from_file(const struct uk_file *file)
{
	return __containerof((struct pipe_node *)file->node,
			     struct uk_pipefile, node);
}

ssize_t uk_pipefile_readable_bytes(const struct uk_file *file,
				   enum uk_pipefile_count_kind kind)
{
	struct uk_pipefile *pipe;
	ssize_t available;

	if (file->vol != PIPE_VOLID ||
	    (kind != UK_PIPEFILE_COUNT_TOTAL &&
	     kind != UK_PIPEFILE_COUNT_PACKET))
		return -EINVAL;
	pipe = pipe_from_file(file);
	uk_mutex_lock(&pipe->lock);
	if (kind == UK_PIPEFILE_COUNT_PACKET) {
		if (pipe->kind == UK_PIPEFILE_POSIX) {
			struct pipe_pages *pages = &pipe->node.pages;
			struct pipe_page *first = &pages->buffers[pages->first];

			available =
			    pages->count && first->packet ? first->length : 0;
		} else {
			available =
			    pipe->node.head ? pipe->node.head->length : 0;
		}
	} else {
		available = pipe->node.used;
	}
	uk_mutex_unlock(&pipe->lock);
	return available;
}

int uk_pipefile_set_capacity(const struct uk_file *file, size_t capacity)
{
	struct uk_pipefile *pipe;

	if (file->vol != PIPE_VOLID || !capacity || capacity > PIPE_SIZE)
		return -EINVAL;
	pipe = pipe_from_file(file);
	if (pipe->kind == UK_PIPEFILE_POSIX && capacity % __PAGE_SIZE)
		return -EINVAL;
	uk_mutex_lock(&pipe->lock);
	if (pipe->kind == UK_PIPEFILE_POSIX &&
	    pipe->node.pages.count > capacity / __PAGE_SIZE) {
		uk_mutex_unlock(&pipe->lock);
		return -EBUSY;
	}
	if (pipe->node.limit != capacity) {
		pipe->node.limit = capacity;
		pipe_events(pipe, UKFD_POLLOUT);
	}
	uk_mutex_unlock(&pipe->lock);
	return 0;
}

ssize_t uk_pipefile_write_packet(const struct uk_file *file,
				 const struct iovec *iov, size_t count,
				 const struct uk_file *account)
{
	struct uk_pipefile *pipe = pipe_from_file(file);
	struct pipe_budget *budget = account->node;
	const struct pipe_end *end = __containerof(file, struct pipe_end, file);
	size_t length = uk_iov_len(iov, count);
	ssize_t result;

	if (file->vol != PIPE_VOLID || pipe->kind != UK_PIPEFILE_SOCKET ||
	    account->vol != PIPE_BUDGET_VOLID)
		return -EINVAL;
	if ((end->mode & O_PATH) || !(end->mode & (O_WRONLY | O_RDWR)))
		return -EBADF;
	uk_mutex_lock(&pipe->lock);
	uk_mutex_lock(&budget->lock);
	if (length > budget->capacity - 32) {
		result = -EMSGSIZE;
	} else if (budget->used > budget->capacity ||
		   length + 32 > budget->capacity - budget->used) {
		result = -EAGAIN;
	} else {
		result = pipe_socket_write(file, iov, count, 0,
					   O_DIRECT | UK_PIPE_WRITE_ATOMIC);
		if (result >= 0) {
			struct pipe_msg *message = pipe->node.tail;

			UK_ASSERT((size_t)result == length);
			uk_file_acquire(account);
			message->budget = account;
			message->charge = length + 32;
			budget->used += message->charge;
		}
	}
	if (result >= 0)
		pipe_budget_events(budget);
	uk_mutex_unlock(&budget->lock);
	if (result >= 0)
		pipe_events(pipe, UKFD_POLLIN);
	uk_mutex_unlock(&pipe->lock);
	return result;
}

static void pipe_discard(struct pipe_node *node)
{
	struct pipe_msg *message = node->head;

	while (message) {
		struct pipe_msg *next = message->next;

		if (message->budget) {
			pipe_budget_return(message->budget, message->charge);
			message->budget = NULL;
		}
		if (next == message)
			break;
		message = next;
	}
	node->head = NULL;
	node->tail = NULL;
	node->used = 0;
	node->pages.first = 0;
	node->pages.count = 0;
	node->free = &node->msgs[0];
	for (int i = 0; i < PIPE_MSGCOUNT - 1; i++)
		node->msgs[i].next = &node->msgs[i + 1];
	node->msgs[PIPE_MSGCOUNT - 1].next = NULL;
}

void uk_pipefile_discard(const struct uk_file *file)
{
	struct uk_pipefile *pipe = pipe_from_file(file);

	UK_ASSERT(file->vol == PIPE_VOLID);
	uk_mutex_lock(&pipe->lock);
	if (pipe->node.head || pipe->node.pages.count) {
		pipe_discard(&pipe->node);
		pipe_events(pipe, UKFD_POLLIN | UKFD_POLLOUT);
	}
	uk_mutex_unlock(&pipe->lock);
}

int uk_pipefile_shutdown_read(const struct uk_file *file)
{
	struct uk_pipefile *pipe = pipe_from_file(file);
	const struct pipe_end *end = __containerof(file, struct pipe_end, file);

	UK_ASSERT(file->vol == PIPE_VOLID);
	if (pipe->kind != UK_PIPEFILE_SOCKET ||
	    (end->mode & O_ACCMODE) != O_RDONLY || (end->mode & O_PATH))
		return -EINVAL;
	uk_mutex_lock(&pipe->lock);
	pipe->node.read_shutdown = true;
	pipe_events(pipe, UKFD_POLLIN | UKFD_POLLOUT);
	uk_mutex_unlock(&pipe->lock);
	return 0;
}

static struct uk_file_io_result pipe_io(const struct uk_file *file,
					const struct iovec *iov, size_t count,
					size_t off, long flags, bool write,
					size_t *packet_size)
{
	struct uk_pipefile *pipe = pipe_from_file(file);
	const struct pipe_end *end = __containerof(file, struct pipe_end, file);
	struct uk_file_io_result result;
	size_t used, pages;
	struct pipe_msg *head, *tail;

	if ((end->mode & O_PATH) ||
	    (write ? !(end->mode & (O_WRONLY | O_RDWR)) :
		     !!(end->mode & O_WRONLY)))
		return uk_file_io_status(-EBADF);
	uk_mutex_lock(&pipe->lock);
	used = pipe->node.used;
	pages = pipe->node.pages.count;
	head = pipe->node.head;
	tail = pipe->node.tail;
	if (pipe->kind == UK_PIPEFILE_POSIX)
		result =
		    pipe_page_io(&pipe->node, iov, count, off, flags, write);
	else
		result = uk_file_io_status(write ?
			pipe_socket_write(file, iov, count, off, flags) :
			pipe_socket_read(file, iov, count, off, flags,
					 packet_size));
	if (used != pipe->node.used || pages != pipe->node.pages.count ||
	    head != pipe->node.head || tail != pipe->node.tail)
		pipe_events(pipe, write ? UKFD_POLLIN : UKFD_POLLOUT);
	uk_mutex_unlock(&pipe->lock);
	return result;
}

ssize_t uk_pipefile_recv(const struct uk_file *file, const struct iovec *iov,
			 size_t count, long flags, size_t *packet_size)
{
	struct uk_file_io_result result;

	if (file->vol != PIPE_VOLID || !packet_size ||
	    pipe_from_file(file)->kind != UK_PIPEFILE_SOCKET)
		return -EINVAL;

	*packet_size = 0;
	result = pipe_io(file, iov, count, 0, flags, false, packet_size);
	return result.count ? (ssize_t)result.count : result.error;
}

static ssize_t pipe_read_end(const struct uk_file *file,
			     const struct iovec *iov, size_t count,
			     size_t off, long flags)
{
	struct uk_file_io_result result =
		pipe_io(file, iov, count, off, flags, false, NULL);

	return result.count ? (ssize_t)result.count : result.error;
}

static struct uk_file_io_result
pipe_write_end(const struct uk_file *file, const struct iovec *iov,
	       size_t count, size_t off, long flags,
	       enum uk_file_write_kind kind __unused)
{
	const struct uk_pipefile *pipe = pipe_from_file(file);
	size_t length = uk_iov_len(iov, count);

	if (pipe->kind == UK_PIPEFILE_POSIX && length &&
	    length <= UK_PIPE_ATOMIC_SIZE)
		flags |= UK_PIPE_WRITE_ATOMIC;
	return pipe_io(file, iov, count, off, flags, true, NULL);
}

static const struct uk_file *pipe_file_open(const struct uk_file *file,
					    unsigned int mode);

static int pipe_ctl(const struct uk_file *file, int family, int request,
		    uintptr_t argument, uintptr_t second __unused,
		    uintptr_t third __unused)
{
	ssize_t bytes;
	int available;

	if (family == UKFILE_CTL_FILE)
		return request == UKFILE_CTL_FILE_SYNC ||
		       request == UKFILE_CTL_FILE_TRUNC ? -EINVAL : -EOPNOTSUPP;
	if (family != UKFILE_CTL_IOCTL || request != FIONREAD)
		return -ENOTTY;
	bytes = uk_pipefile_readable_bytes(file, UK_PIPEFILE_COUNT_TOTAL);
	if (bytes < 0)
		return bytes;
	if (bytes > INT_MAX)
		return -EOVERFLOW;
	available = bytes;
	return uk_nofault_try_write(available, (int *)argument) ? 0 : -EFAULT;
}

static const struct uk_file_ops pipe_ops = {.read = pipe_read_end,
					    .write = pipe_write_end,
					    .mem = uk_file_nop_mem,
					    .getstat = pipe_getstat,
					    .setstat = uk_file_nop_setstat,
					    .ctl = pipe_ctl,
					    .open = pipe_file_open};

static void pipe_release(const struct uk_file *file, int what)
{
	struct uk_pipefile *pipe = pipe_from_file(file);
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
		if (!pipe->node.readers && !pipe->node.writers)
			pipe_discard(&pipe->node);
		pipe_events(pipe, end->mode & O_PATH ? 0 :
			    UKFD_POLLIN | UKFD_POLLOUT);
	}
	if (what & UK_FILE_RELEASE_OBJ) {
		last = !--pipe->references;
		if (last)
			pipe_discard(&pipe->node);
	}
	uk_mutex_unlock(&pipe->lock);
	if (what & UK_FILE_RELEASE_OBJ)
		uk_free(allocator, end);
	if (last)
		uk_free(allocator, pipe);
}

struct uk_pipefile *uk_pipefile_alloc(enum uk_pipefile_kind kind)
{
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct uk_pipefile *pipe = uk_zalloc(allocator, sizeof(*pipe));

	if (!pipe)
		return NULL;
	pipe->kind = kind;
	pipe->alloc = allocator;
	pipe->references = 1;
	pipe->node.limit = PIPE_SIZE;
	uk_mutex_init(&pipe->lock);
	UK_INIT_LIST_HEAD(&pipe->ends);
	pipe->node.free = &pipe->node.msgs[0];
	for (int i = 0; i < PIPE_MSGCOUNT - 1; i++)
		pipe->node.msgs[i].next = &pipe->node.msgs[i + 1];
	return pipe;
}

void uk_pipefile_release(struct uk_pipefile *pipe)
{
	struct uk_alloc *allocator = pipe->alloc;
	bool last;

	uk_mutex_lock(&pipe->lock);
	last = !--pipe->references;
	uk_mutex_unlock(&pipe->lock);
	if (last)
		uk_free(allocator, pipe);
}

static struct pipe_end *pipe_open(struct uk_pipefile *pipe, unsigned int mode,
				  bool named)
{
	struct pipe_end *end = uk_zalloc(pipe->alloc, sizeof(*end));

	if (!end)
		return ERR2PTR(-ENOMEM);
	end->references = UK_FILE_REFCNT_INIT_VALUE(end->references);
	end->state = UK_FILE_STATE_INIT_VALUE(end->state);
	end->mode = mode;
	end->named = named;
	end->file = (struct uk_file){
		.vol = PIPE_VOLID,
		.node = &pipe->node,
		.refcnt = &end->references,
		.state = &end->state,
		.ops = &pipe_ops,
		.write_sigpipe = true,
		.nofault_io = true,
		._release = pipe_release
	};
	uk_mutex_lock(&pipe->lock);
	if (named && (mode & O_NONBLOCK) && (mode & O_ACCMODE) == O_WRONLY &&
	    !pipe->node.readers) {
		uk_mutex_unlock(&pipe->lock);
		uk_free(pipe->alloc, end);
		return ERR2PTR(-ENXIO);
	}
	end->peer_present = (mode & O_ACCMODE) == O_RDONLY ?
		pipe->node.writers != 0 : pipe->node.readers != 0;
	end->peer_arrival = (mode & O_ACCMODE) == O_RDONLY ?
		pipe->writers_arrived : pipe->readers_arrived;
	pipe->references++;
	if (!(mode & O_PATH)) {
		if (!(mode & O_WRONLY)) {
			if (named && (mode & O_NONBLOCK) && !pipe->node.writers)
				end->writers_seen = pipe->writers_arrived;
			pipe->readers_arrived++;
			pipe->node.readers++;
		}
		if (mode & (O_WRONLY | O_RDWR)) {
			pipe->writers_arrived++;
			pipe->node.writers++;
		}
	}
	uk_list_add_tail(&end->link, &pipe->ends);
	pipe_events(pipe, mode & O_PATH ? 0 : UKFD_POLLIN | UKFD_POLLOUT);
	uk_mutex_unlock(&pipe->lock);
	return end;
}

static const struct uk_file *pipe_file_open(const struct uk_file *file,
					    unsigned int mode)
{
	struct pipe_end *end = pipe_open(pipe_from_file(file), mode, false);

	return PTRISERR(end) ? ERR2PTR(PTR2ERR(end)) : &end->file;
}

static bool pipe_peer_arrived(struct uk_pipefile *pipe,
			      const struct pipe_end *end)
{
	if ((end->mode & O_ACCMODE) == O_RDONLY)
		return pipe->node.writers ||
			pipe->writers_arrived != end->peer_arrival;
	return pipe->node.readers ||
		pipe->readers_arrived != end->peer_arrival;
}

const struct uk_file *uk_pipefile_open_named(struct uk_pipefile *pipe,
					     unsigned int mode)
{
	struct pipe_end *end = pipe_open(pipe, mode, true);
	struct uk_fdio_wait_context wait;
	int rc;

	if (PTRISERR(end))
		return ERR2PTR(PTR2ERR(end));
	if ((mode & (O_PATH | O_NONBLOCK)) || (mode & O_ACCMODE) == O_RDWR ||
	    end->peer_present)
		return &end->file;
	wait = uk_fdio_wait_init(&end->file, UKFD_POLLIN | UKFD_POLLOUT);
	for (;;) {
		bool ready;

		uk_fdio_wait_snapshot(&wait);
		uk_mutex_lock(&pipe->lock);
		ready = pipe_peer_arrived(pipe, end);
		uk_mutex_unlock(&pipe->lock);
		if (ready)
			return &end->file;
		rc = uk_fdio_wait(&wait);
		if (rc) {
			uk_mutex_lock(&pipe->lock);
			ready = pipe_peer_arrived(pipe, end);
			uk_mutex_unlock(&pipe->lock);
			if (ready)
				return &end->file;
			uk_file_release(&end->file);
			return ERR2PTR(rc);
		}
	}
}

int uk_pipefile_create(struct uk_file *pipes[2], enum uk_pipefile_kind kind)
{
	struct uk_pipefile *pipe = uk_pipefile_alloc(kind);
	struct pipe_end *reader, *writer;
	int error = 0;

	if (!pipe)
		return -ENOMEM;
	reader = pipe_open(pipe, O_RDONLY, false);
	if (PTRISERR(reader)) {
		error = PTR2ERR(reader);
		goto out;
	}
	writer = pipe_open(pipe, O_WRONLY, false);
	if (PTRISERR(writer)) {
		error = PTR2ERR(writer);
		uk_file_release(&reader->file);
		goto out;
	}
	pipes[0] = &reader->file;
	pipes[1] = &writer->file;
out:
	uk_pipefile_release(pipe);
	return error;
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

	r = uk_pipefile_create(pipes, UK_PIPEFILE_POSIX);
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
