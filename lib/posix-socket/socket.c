/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Alexander Jung <alexander.jung@neclab.eu>
 *          Marc Rittinghaus <marc.rittinghaus@kit.edu>
 *          Andrei Tatar <andrei@unikraft.io>
 *
 * Copyright (c) 2020, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2022, Karlsruhe Institute of Technology (KIT).
 *                     All rights reserved.
 * Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <limits.h>

#include <uk/socket.h>
#include <uk/alloc.h>
#include <uk/arch/limits.h>
#include <uk/file.h>
#include <uk/file/nops.h>
#include <uk/posix-fd.h>
#include <uk/posix-fdio.h>
#include <uk/plat/time.h>
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif
#include <uk/nofault.h>
#include <uk/errptr.h>
#include <uk/print.h>
#include <uk/trace.h>
#include <uk/syscall.h>
#include <uk/essentials.h>
#include <errno.h>

#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>
#endif /* CONFIG_LIBPOSIX_FDTAB */

#include "events.h"


static const char POSIX_SOCKET_VOLID[] = "posix_socket_vol";

#define POSIX_SOCKET_FNAME "socket"
#define POSIX_SOCKET_FNAME_LEN (sizeof(POSIX_SOCKET_FNAME) - 1)

#define POSIX_SOCKET_ACCT_FNAME "socket:accepted"
#define POSIX_SOCKET_ACCT_FNAME_LEN (sizeof(POSIX_SOCKET_ACCT_FNAME) - 1)

#define POSIX_SOCKET_PAIR_FNAME "socket:pair"
#define POSIX_SOCKET_PAIR_FNAME_LEN (sizeof(POSIX_SOCKET_PAIR_FNAME) - 1)

#define SOCKET_MODE (O_RDWR|UKFD_O_NOSEEK|UKFD_O_NOIOLOCK)

#define _ERR_BLOCK(r) ((r) == -EAGAIN || (r) == -EWOULDBLOCK)
#define _SHOULD_BLOCK(m) !((m) & O_NONBLOCK)

struct socket_alloc {
	struct uk_file f;
	uk_file_refcnt fref;
	struct uk_file_state fstate;
	struct posix_socket_node node;
#if CONFIG_LIBPOSIX_SOCKET_EVENTS
	struct uk_socket_event_data evd;
#endif /* CONFIG_LIBPOSIX_SOCKET_EVENTS */
};

#if CONFIG_LIBPOSIX_FDTAB
static struct uk_ofile *socketfd_get(int fd)
{
	struct uk_ofile *of = uk_fdtab_get(fd);

	if (unlikely(!of))
		return ERR2PTR(-EBADF);
	if (unlikely(of->file->vol != POSIX_SOCKET_VOLID)) {
		uk_ofile_release(of);
		return ERR2PTR(-ENOTSOCK);
	}
	return of;
}
#endif /* CONFIG_LIBPOSIX_FDTAB */

static ssize_t socket_read(const struct uk_file *sock, const struct iovec *iov,
			   size_t iovcnt, size_t off, long flags)
{
	ssize_t ret;
	int receive_flags = flags & O_NONBLOCK ? MSG_DONTWAIT : 0;
	struct posix_socket_driver *d;

	if (unlikely(sock->vol != POSIX_SOCKET_VOLID))
		return -EINVAL;
	if (unlikely(off))
		return -ESPIPE;

	d = posix_sock_get_driver(sock);
	uk_file_rlock(sock);
	if (d->ops->read) {
		ret = posix_socket_read(sock, iov, iovcnt, receive_flags);
	} else {
		struct msghdr msg = {
			.msg_name = NULL,
			.msg_namelen = 0,
			.msg_iov = (struct iovec *)iov,
			.msg_iovlen = iovcnt,
			.msg_control = NULL,
			.msg_controllen = 0
		};
		ret = posix_socket_recvmsg(sock, &msg, receive_flags);
	}
	uk_file_runlock(sock);
	return ret;
}

static struct uk_file_io_result
socket_write(const struct uk_file *sock, const struct iovec *iov, size_t iovcnt,
	     size_t off, long flags __unused,
	     enum uk_file_write_kind kind __unused)
{
	ssize_t ret;
	struct posix_socket_driver *d;

	if (unlikely(sock->vol != POSIX_SOCKET_VOLID))
		return uk_file_io_status(-EINVAL);
	if (unlikely(off))
		return uk_file_io_status(-ESPIPE);

	d = posix_sock_get_driver(sock);
	uk_file_rlock(sock);
	if (d->ops->write) {
		ret = posix_socket_write(sock, iov, iovcnt);
	} else {
		struct msghdr msg = {
			.msg_name = NULL,
			.msg_namelen = 0,
			.msg_iov = (struct iovec *)iov,
			.msg_iovlen = iovcnt,
			.msg_control = NULL,
			.msg_controllen = 0
		};
		ret = posix_socket_sendmsg(sock, &msg, 0);
	}
	uk_file_runlock(sock);
	return uk_file_io_status(ret);
}

static __nsec socket_io_timeout(const struct uk_file *sock, uk_pollevent events)
{
	struct posix_socket_node *node = posix_sock_get_node(sock);
	__u64 milliseconds;

	uk_file_rlock(sock);
	milliseconds = events & UKFD_POLLIN ? node->receive_timeout_ms
					    : node->send_timeout_ms;
	uk_file_runlock(sock);
	if (milliseconds == __U64_MAX)
		return __NSEC_MAX;
	return milliseconds > __SNSEC_MAX / 1000000 ? __SNSEC_MAX
						    : milliseconds * 1000000;
}

static int
socket_ctl(const struct uk_file *sock, int fam, int req,
	   uintptr_t arg1, uintptr_t arg2 __unused, uintptr_t arg3 __unused)
{
	switch (fam) {
	case UKFILE_CTL_IOCTL:
	{
		struct posix_socket_driver *driver;
		int ret;

		if (req == SIOCGIFNAME || req == SIOCGIFINDEX) {
			driver = posix_socket_driver_get(AF_INET);
			if (!driver || !driver->ops->interface_ioctl)
				driver = posix_socket_driver_get(AF_INET6);
			return driver && driver->ops->interface_ioctl
				       ? driver->ops->interface_ioctl(req, (void *)arg1)
				       : -ENOTTY;
		}
		uk_file_rlock(sock);
		ret = posix_socket_ioctl(sock, req, (void *)arg1);
		uk_file_runlock(sock);
		return ret;
	}
	default:
		return -ENOSYS;
	}
}

#define SOCKET_STATX_MASK \
	(UK_STATX_TYPE|UK_STATX_MODE|UK_STATX_NLINK|UK_STATX_INO|UK_STATX_SIZE)

static int
socket_getstat(const struct uk_file *sock,
	       unsigned int mask __unused, struct uk_statx *arg)
{
	/* Since all information is immediately available, ignore mask arg */
	arg->stx_mask = SOCKET_STATX_MASK;
	arg->stx_mode = S_IFSOCK|0777;
	arg->stx_nlink = 1;
	arg->stx_ino = (uintptr_t)sock; /* Must be unique per-socket */
	arg->stx_size = 0;

	/* Following fields are always filled in, not in stx_mask */
	arg->stx_dev_major = 0;
	arg->stx_dev_minor = 8; /* Same value Linux returns for sockets */
	arg->stx_rdev_major = 0;
	arg->stx_rdev_minor = 0;
	arg->stx_blksize = 0x1000;
	return 0;
}

static const struct uk_file_ops socket_file_ops = {
	.read = socket_read,
	.write = socket_write,
	.mem = uk_file_nop_mem,
	.getstat = socket_getstat,
	.setstat = uk_file_nop_setstat,
	.io_timeout = socket_io_timeout,
	.ctl = socket_ctl
};

static void socket_release(const struct uk_file *sock, int what)
{
	UK_ASSERT(sock->vol == POSIX_SOCKET_VOLID);
	if (what & UK_FILE_RELEASE_RES) {
		posix_socket_close(sock);
	}
	if (what & UK_FILE_RELEASE_OBJ) {
		struct socket_alloc *al = __containerof(sock,
							struct socket_alloc, f);

		/* Raise a CLOSE event only if we ever raised an event on this
		 * socket already
		 */
		if (uk_socket_event_has_raised(&al->evd))
			uk_socket_event_raise(&al->evd, CLOSE);
		uk_free(al->node.driver->allocator, al);
	}
}


#if CONFIG_LIBPOSIX_SOCKET_POLLED
static uk_pollevent socket_poll(posix_sock *sock, uk_pollevent mask)
{
	struct posix_socket_driver *driver = posix_sock_get_driver(sock);
	uk_pollevent events = driver->ops->poll(sock, mask);

	if (uk_load_n(&posix_sock_get_node(sock)->pending_error))
		events |= EPOLLERR;
	return events & mask;
}
#endif

static void _socket_init(struct socket_alloc *al,
			 struct posix_socket_driver *d, void *sock_data,
			 bool write_sigpipe)
{
	al->node = (struct posix_socket_node){
		.sock_data = sock_data,
		.driver = d,
		.receive_timeout_ms = __U64_MAX,
		.send_timeout_ms = __U64_MAX,
		.event_lock = UK_MUTEX_INITIALIZER(al->node.event_lock)
	};
#if CONFIG_LIBPOSIX_SOCKET_POLLED
	if (d->ops->poll)
		al->fstate = UK_FILE_POLLED_STATE_INIT_VALUE(al->fstate,
							     socket_poll);
	else
#endif /* CONFIG_LIBPOSIX_SOCKET_POLLED */
		al->fstate = UK_FILE_STATE_INIT_VALUE(al->fstate);
	al->fref = UK_FILE_REFCNT_INIT_VALUE(al->fref);
	al->f = (struct uk_file){
		.vol = POSIX_SOCKET_VOLID,
		.node = &al->node,
		.ops = &socket_file_ops,
		.write_sigpipe = write_sigpipe,
		.nofault_io = d->ops->nofault_io,
		.refcnt = &al->fref,
		.state = &al->fstate,
		._release = socket_release
	};
	posix_socket_poll_setup(&al->f);
}


struct uk_file *uk_socket_create(int family, int type, int protocol)
{
	struct posix_socket_driver *d = posix_socket_driver_get(family);
	struct socket_alloc *al;
	void *sock_data;

	if (unlikely(!d))
		return ERR2PTR(-EAFNOSUPPORT);

	al = uk_malloc(d->allocator, sizeof(*al));
	if (unlikely(!al))
		return ERR2PTR(-ENOMEM);

	sock_data = posix_socket_create(d, family, type, protocol);
	/* NULL is a valid return value on success */
	if (unlikely(sock_data && PTRISERR(sock_data))) {
		uk_free(d->allocator, al);
		return sock_data;
	}

	_socket_init(al, d, sock_data,
		     (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_STREAM);
	uk_socket_evd_init(&al->evd, family, type, protocol);
	uk_socket_event_raise(&al->evd, CREATE);
	return &al->f;
}

/* Internal API & Syscalls */
#if CONFIG_LIBPOSIX_FDTAB
int uk_sys_socket(int family, int type, int protocol)
{
	int fd;
	unsigned int mode = SOCKET_MODE;
	struct uk_file *sock = uk_socket_create(family, type, protocol);

	if (unlikely(PTRISERR(sock)))
		return PTR2ERR(sock);

	if (type & SOCK_NONBLOCK)
		mode |= O_NONBLOCK;
	if (type & SOCK_CLOEXEC)
		mode |= O_CLOEXEC;
	fd = uk_fdtab_open_named(sock, mode, POSIX_SOCKET_FNAME,
				 POSIX_SOCKET_FNAME_LEN);
	uk_file_release(sock);
	return fd;
}


UK_TRACEPOINT(trace_posix_socket_create, "%d %d %d", int, int, int);
UK_TRACEPOINT(trace_posix_socket_create_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_create_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, socket, int, family, int, type, int, protocol)
{
	int ret;

	trace_posix_socket_create(family, type, protocol);

	ret = uk_sys_socket(family, type, protocol);

	if (ret >= 0)
		trace_posix_socket_create_ret(ret);
	else
		trace_posix_socket_create_err(ret);

	return ret;
}
#endif /* CONFIG_LIBPOSIX_FDTAB */

const struct uk_file *uk_socket_accept(const struct uk_file *sock, int blocking,
				       struct sockaddr *addr,
				       socklen_t *addr_len, int flags)
{
	void *new_data;
	struct uk_fdio_wait_context wait;
	struct socket_alloc *al;
	struct socket_alloc *al_listener __maybe_unused;
	struct posix_socket_node *n = (struct posix_socket_node *)sock->node;

	al = uk_malloc(n->driver->allocator, sizeof(*al));
	if (unlikely(!al))
		return ERR2PTR(-ENOMEM);

	wait = uk_fdio_wait_init(sock, UKFD_POLLIN);

	for (;;) {
		uk_fdio_wait_snapshot(&wait);
		uk_file_rlock(sock);
		new_data = posix_socket_accept4(sock, addr, addr_len, flags);
		uk_file_runlock(sock);
		if (!blocking ||
		    !PTRISERR(new_data) || !_ERR_BLOCK(PTR2ERR(new_data)))
			break;
		int rc = uk_fdio_wait(&wait);

		if (rc < 0) {
			new_data = ERR2PTR(rc);
			break;
		}
	}
	if (unlikely(PTRISERR(new_data))) {
		uk_free(n->driver->allocator, al);
		return new_data;
	}

	_socket_init(al, n->driver, new_data, sock->write_sigpipe);

	al_listener = __containerof(sock, struct socket_alloc, f);
#if CONFIG_LIBPOSIX_SOCKET_EVENTS
	uk_socket_evd_init_from(&al->evd, &al_listener->evd);
	uk_socket_evd_laddr_set_from(&al->evd, &al_listener->evd);
	uk_socket_evd_raddr_set(&al->evd, addr, (addr_len ? *addr_len : 0));
#endif /* CONFIG_LIBPOSIX_SOCKET_EVENTS */

	return &al->f;
}

#if CONFIG_LIBPOSIX_FDTAB
static int socket_address_copy(const struct sockaddr_storage *address,
			       socklen_t length, struct sockaddr *user,
			       socklen_t *user_length);

int uk_sys_accept(const struct uk_file *sock, int blocking,
		  struct sockaddr *addr, socklen_t *addr_len, int flags)
{
	struct uk_fdtab_reservation *reservation;
	struct sockaddr_storage address = {0};
	socklen_t length = sizeof(address);
	const struct uk_file *sockfile;
	struct uk_ofile *opened;
	unsigned int mode = SOCKET_MODE;
	int result;

	if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
		return -EINVAL;
	if (flags & SOCK_NONBLOCK)
		mode |= O_NONBLOCK;
	if (flags & SOCK_CLOEXEC)
		mode |= O_CLOEXEC;
	reservation = uk_fdtab_reserve(mode);
	if (PTRISERR(reservation))
		return PTR2ERR(reservation);
	if (sock->vol != POSIX_SOCKET_VOLID) {
		result = -ENOTSOCK;
		goto cancel;
	}
	opened = uk_malloc(uk_alloc_get_default(),
			   UKFD_OFILE_SIZE(POSIX_SOCKET_ACCT_FNAME_LEN));
	if (!opened) {
		result = -ENOMEM;
		goto cancel;
	}
	sockfile = uk_socket_accept(sock, blocking, (struct sockaddr *)&address,
				    &length, flags);
	if (PTRISERR(sockfile)) {
		result = PTR2ERR(sockfile);
		uk_free(uk_alloc_get_default(), opened);
		goto cancel;
	}
	uk_ofile_init(opened, sockfile, mode | UKFD_O_NAMED);
	memcpy(opened->name, POSIX_SOCKET_ACCT_FNAME,
	       sizeof(POSIX_SOCKET_ACCT_FNAME));
	if (addr) {
		result = socket_address_copy(&address, length, addr, addr_len);
		if (result) {
			uk_ofile_release(opened);
			goto cancel;
		}
	}
	result = uk_fdtab_install(reservation, opened, NULL);
	reservation = NULL;
	if (result < 0) {
		uk_ofile_release(opened);
	} else {
		struct socket_alloc *al __maybe_unused;

		al = __containerof(sock, struct socket_alloc, f);
		uk_socket_event_raise(&al->evd, ACCEPT);
	}
cancel:
	uk_fdtab_cancel(reservation);
	return result;
}

UK_TRACEPOINT(trace_posix_socket_accept, "%d %p %p", int,
	      struct sockaddr *restrict, socklen_t *restrict);
UK_TRACEPOINT(trace_posix_socket_accept_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_accept_err, "%d", int);

int do_accept4(int sock, struct sockaddr *addr, socklen_t *addr_len, int flags)
{
	int ret;
	unsigned int mode;
	struct uk_ofile *of;

	trace_posix_socket_accept(sock, addr, addr_len);

	of = uk_fdtab_get(sock);
	if (!of) {
		ret = -EBADF;
		goto out;
	}

	if (of->mode & O_PATH) {
		ret = -EBADF;
		uk_ofile_release(of);
		goto out;
	}
	mode = of->mode;
	ret =
	    uk_sys_accept(of->file, _SHOULD_BLOCK(mode), addr, addr_len, flags);
	uk_ofile_release(of);

out:
	if (ret >= 0)
		trace_posix_socket_accept_ret(ret);
	else
		trace_posix_socket_accept_err(ret);
	return ret;
}

UK_SYSCALL_R_DEFINE(int, accept, int, sock, struct sockaddr *, addr,
		    socklen_t *, addr_len)
{
	return do_accept4(sock, addr, addr_len, 0);
}

UK_SYSCALL_R_DEFINE(int, accept4, int, sock, struct sockaddr *, addr,
		    socklen_t *, addr_len, int, flags)
{
	return do_accept4(sock, addr, addr_len, flags);
}

static struct uk_ofile *socket_address_get(int fd,
					   struct sockaddr_storage *address,
					   const struct sockaddr *user,
					   socklen_t length)
{
	struct uk_ofile *of = socketfd_get(fd);
	int error = 0;

	if (PTRISERR(of))
		return of;
	if (length > sizeof(*address))
		error = -EINVAL;
	else if (uk_nofault_memcpy((char *)address, (const char *)user, length,
				   0) != length)
		error = -EFAULT;
	if (error) {
		uk_ofile_release(of);
		return ERR2PTR(error);
	}
	return of;
}

static int socket_address_copy(const struct sockaddr_storage *address,
			       socklen_t length, struct sockaddr *user,
			       socklen_t *user_length)
{
	socklen_t capacity;

	UK_ASSERT(length <= sizeof(*address));
	if (uk_nofault_memcpy((char *)&capacity, (const char *)user_length,
			      sizeof(capacity), 0) != sizeof(capacity))
		return -EFAULT;
	if (capacity > INT_MAX)
		return -EINVAL;
	if (uk_nofault_memcpy((char *)user_length, (const char *)&length,
			      sizeof(length), 0) != sizeof(length))
		return -EFAULT;
	length = MIN(capacity, length);
	if (uk_nofault_memcpy((char *)user, (const char *)address, length, 0)
	    != length)
		return -EFAULT;
	return 0;
}

UK_TRACEPOINT(trace_posix_socket_bind, "%d %p %d",
	      int, const struct sockaddr *, socklen_t);
UK_TRACEPOINT(trace_posix_socket_bind_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_bind_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, bind, int, sock, const struct sockaddr *, addr,
		    socklen_t, addr_len)
{
	int ret;
	struct uk_ofile *of;
	struct sockaddr_storage address = {0};

	trace_posix_socket_bind(sock, addr, addr_len);

	of = socket_address_get(sock, &address, addr, addr_len);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	uk_file_wlock(of->file);
	ret = posix_socket_bind(of->file,
				(const struct sockaddr *)&address, addr_len);
	uk_file_wunlock(of->file);

out:
	if (ret) {
		trace_posix_socket_bind_err(ret);
	} else {
		struct socket_alloc *al __maybe_unused;

		al = __containerof(of->file, struct socket_alloc, f);
		uk_socket_evd_laddr_set(&al->evd,
					(const struct sockaddr *)&address,
					addr_len);
		uk_socket_event_raise(&al->evd, BIND);
		trace_posix_socket_bind_ret(ret);
	}

	if (!PTRISERR(of))
		uk_ofile_release(of);
	return ret;
}


UK_TRACEPOINT(trace_posix_socket_shutdown, "%d %d", int, int);
UK_TRACEPOINT(trace_posix_socket_shutdown_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_shutdown_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, shutdown, int, sock, int, how)
{
	int ret;
	struct uk_ofile *of;

	trace_posix_socket_shutdown(sock, how);

	of = socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	if (unlikely(!(how == SHUT_RD || how == SHUT_WR || how == SHUT_RDWR))) {
		uk_ofile_release(of);
		ret = -EINVAL;
		goto out;
	}

	uk_file_wlock(of->file);
	ret = posix_socket_shutdown(of->file, how);
	uk_file_wunlock(of->file);
	uk_ofile_release(of);

out:
	if (ret)
		trace_posix_socket_shutdown_err(ret);
	else
		trace_posix_socket_shutdown_ret(ret);
	return ret;
}

static int socket_name(int fd, struct sockaddr *user, socklen_t *user_length,
		       bool peer)
{
	struct uk_ofile *of = socketfd_get(fd);
	struct sockaddr_storage address = {0};
	struct sockaddr *output = (struct sockaddr *)&address;
	socklen_t length = sizeof(address);
	int ret;

	if (PTRISERR(of))
		return PTR2ERR(of);
	uk_file_rlock(of->file);
	ret = peer ? posix_socket_getpeername(of->file, output, &length) :
		     posix_socket_getsockname(of->file, output, &length);
	uk_file_runlock(of->file);
	uk_ofile_release(of);
	return ret ? ret : socket_address_copy(&address, length,
					      user, user_length);
}

UK_TRACEPOINT(trace_posix_socket_getpeername, "%d %p %p", int,
	      struct sockaddr *restrict, socklen_t *restrict);
UK_TRACEPOINT(trace_posix_socket_getpeername_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_getpeername_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, getpeername, int, sock,
		    struct sockaddr *restrict, addr,
		    socklen_t *restrict, addr_len)
{
	int ret;

	trace_posix_socket_getpeername(sock, addr, addr_len);
	ret = socket_name(sock, addr, addr_len, true);

	if (ret)
		trace_posix_socket_getpeername_err(ret);
	else
		trace_posix_socket_getpeername_ret(ret);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_getsockname, "%d %p %p",
	      int, struct sockaddr *restrict, socklen_t *restrict);
UK_TRACEPOINT(trace_posix_socket_getsockname_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_getsockname_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, getsockname, int, sock,
		    struct sockaddr *restrict, addr,
		    socklen_t *restrict, addr_len)
{
	int ret;

	trace_posix_socket_getsockname(sock, addr, addr_len);
	ret = socket_name(sock, addr, addr_len, false);

	if (ret)
		trace_posix_socket_getsockname_err(ret);
	else
		trace_posix_socket_getsockname_ret(ret);
	return ret;
}

#define LINUX_SO_RCVTIMEO_NEW 66
#define LINUX_SO_SNDTIMEO_NEW 67

static bool socket_timeout_option(int level, int name)
{
	return level == SOL_SOCKET &&
	       (name == SO_RCVTIMEO || name == SO_SNDTIMEO ||
		name == LINUX_SO_RCVTIMEO_NEW || name == LINUX_SO_SNDTIMEO_NEW);
}

static __u64 *socket_timeout_value(posix_sock *sock, int name)
{
	struct posix_socket_node *node = posix_sock_get_node(sock);

	return name == SO_RCVTIMEO || name == LINUX_SO_RCVTIMEO_NEW
		   ? &node->receive_timeout_ms
		   : &node->send_timeout_ms;
}

static int socket_timeout_get(posix_sock *sock, int name, void *value,
			      socklen_t *length)
{
	__u64 milliseconds = *socket_timeout_value(sock, name);
	struct timeval timeout = {0};
	size_t size = MIN(*length, sizeof(timeout));

	if (milliseconds != __U64_MAX) {
		timeout.tv_sec = milliseconds / 1000;
		timeout.tv_usec = (milliseconds % 1000) * 1000;
	}
	if (uk_nofault_memcpy(value, (const char *)&timeout, size, 0) != size)
		return -EFAULT;
	*length = size;
	return 0;
}

static int socket_timeout_set(posix_sock *sock, int name, const void *value,
			      socklen_t length)
{
	struct timeval timeout;
	__u64 milliseconds;
	int initial;

	if (length < sizeof(initial))
		return -EINVAL;
	if (!uk_nofault_try_read(initial, value))
		return -EFAULT;
	if (length < sizeof(timeout))
		return -EINVAL;
	if (!uk_nofault_try_read(timeout, value))
		return -EFAULT;
	if (timeout.tv_usec < 0 || timeout.tv_usec >= 1000000)
		return -EDOM;
	if (timeout.tv_sec < 0)
		milliseconds = 0;
	else if ((!timeout.tv_sec && !timeout.tv_usec) ||
		 (__u64)timeout.tv_sec >= __S64_MAX / 1000 - 1)
		milliseconds = __U64_MAX;
	else
		milliseconds = (__u64)timeout.tv_sec * 1000 +
			       (timeout.tv_usec + 999) / 1000;
	struct posix_socket_node *node = posix_sock_get_node(sock);
	const struct posix_socket_ops *ops = node->driver->ops;

	if (ops->timeout_update) {
		enum posix_socket_timeout direction =
		    name == SO_RCVTIMEO || name == LINUX_SO_RCVTIMEO_NEW
			? POSIX_SOCKET_TIMEOUT_RECEIVE
			: POSIX_SOCKET_TIMEOUT_SEND;
		int rc = ops->timeout_update(sock, direction, milliseconds);

		if (rc)
			return rc;
	}
	*socket_timeout_value(sock, name) = milliseconds;
	return 0;
}

UK_TRACEPOINT(trace_posix_socket_getsockopt, "%d %d %d %p %d",
	      int, int, int, void *, socklen_t *);
UK_TRACEPOINT(trace_posix_socket_getsockopt_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_getsockopt_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, getsockopt, int, sock, int, level, int, optname,
		    void *restrict, optval, socklen_t *restrict, optlen)
{
	int ret;
	socklen_t length;
	struct uk_ofile *of;

	trace_posix_socket_getsockopt(sock, level, optname, optval, optlen);

	of = socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	if (!uk_nofault_try_read(length, optlen)) {
		ret = -EFAULT;
		goto release;
	}
	if (length > INT_MAX) {
		ret = -EINVAL;
		goto release;
	}
	uk_file_rlock(of->file);
	ret = socket_timeout_option(level, optname) ?
		socket_timeout_get(of->file, optname, optval, &length) :
		posix_socket_getsockopt(of->file, level, optname,
					optval, &length);
	uk_file_runlock(of->file);
	if (!ret && !uk_nofault_try_write(length, optlen))
		ret = -EFAULT;
release:
	uk_ofile_release(of);

out:
	if (ret)
		trace_posix_socket_getsockopt_err(ret);
	else
		trace_posix_socket_getsockopt_ret(ret);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_setsockopt, "%d %d %d %p %d",
	      int, int, int, const void *, socklen_t);
UK_TRACEPOINT(trace_posix_socket_setsockopt_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_setsockopt_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, setsockopt, int, sock, int, level, int, optname,
		    const void *, optval, socklen_t, optlen)
{
	int ret;
	struct uk_ofile *of;

	trace_posix_socket_setsockopt(sock, level, optname, optval, optlen);

	of = socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	if (optlen > INT_MAX) {
		ret = -EINVAL;
		uk_ofile_release(of);
		goto out;
	}
	uk_file_wlock(of->file);
	ret = socket_timeout_option(level, optname) ?
		socket_timeout_set(of->file, optname, optval, optlen) :
		posix_socket_setsockopt(of->file, level, optname,
					optval, optlen);
	uk_file_wunlock(of->file);
	uk_ofile_release(of);

out:
	if (ret)
		trace_posix_socket_setsockopt_err(ret);
	else
		trace_posix_socket_setsockopt_ret(ret);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_connect, "%d %p %d",
	      int, const struct sockaddr *, socklen_t);
UK_TRACEPOINT(trace_posix_socket_connect_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_connect_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, connect, int, sock, const struct sockaddr *, addr,
		    socklen_t, addr_len)
{
	int ret;
	unsigned int mode;
	struct uk_fdio_wait_context wait;
	struct uk_ofile *of;
	struct sockaddr_storage address = {0};

	trace_posix_socket_connect(sock, addr, addr_len);

	of = socket_address_get(sock, &address, addr, addr_len);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	mode = of->mode;
	wait = uk_fdio_wait_init(of->file, UKFD_POLLOUT);
	uk_fdio_wait_snapshot(&wait);
	uk_file_wlock(of->file);
	ret = posix_socket_connect(of->file,
				   (const struct sockaddr *)&address, addr_len);
	uk_file_wunlock(of->file);
	if (ret == -EINPROGRESS && _SHOULD_BLOCK(mode)) {
		socklen_t _opsz = sizeof(ret);

		ret = uk_fdio_wait(&wait);
		if (ret == -EAGAIN)
			ret = -EINPROGRESS;
		if (!ret) {
			int error;

			uk_file_rlock(of->file);
			ret = posix_socket_getsockopt(of->file, SOL_SOCKET,
						      SO_ERROR, &error, &_opsz);
			uk_file_runlock(of->file);
			if (!ret)
				ret = -error;
		}
	}

out:
	if (ret && ret != -EINPROGRESS) {
		trace_posix_socket_connect_err(ret);
	} else {
		struct socket_alloc *al __maybe_unused;

		al = __containerof(of->file, struct socket_alloc, f);
		uk_socket_evd_raddr_set(&al->evd,
					(const struct sockaddr *)&address,
					addr_len);
		uk_socket_event_raise(&al->evd, CONNECT);
		trace_posix_socket_connect_ret(ret);
	}
	if (!PTRISERR(of))
		uk_ofile_release(of);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_listen, "%d %d", int, int);
UK_TRACEPOINT(trace_posix_socket_listen_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_listen_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, listen, int, sock, int, backlog)
{
	int ret;
	struct uk_ofile *of;

	if ((unsigned int)backlog > SOMAXCONN)
		backlog = SOMAXCONN;

	trace_posix_socket_listen(sock, backlog);

	of = socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	uk_file_wlock(of->file);
	ret = posix_socket_listen(of->file, backlog);
	uk_file_wunlock(of->file);

out:
	if (ret) {
		trace_posix_socket_listen_err(ret);
	} else {
		struct socket_alloc *al __maybe_unused;

		al = __containerof(of->file, struct socket_alloc, f);
		uk_socket_event_raise(&al->evd, LISTEN);
		trace_posix_socket_listen_ret(ret);
	}
	if (!PTRISERR(of))
		uk_ofile_release(of);
	return ret;
}


static bool socket_io_retry(const struct uk_fdio_wait_context *wait,
			    unsigned int mode, int flags, ssize_t *result)
{
	int rc;

	if (!_SHOULD_BLOCK(mode) || (flags & MSG_DONTWAIT) ||
	    !_ERR_BLOCK(*result))
		return false;
	rc = uk_fdio_wait(wait);
	if (rc < 0) {
		*result = rc;
		return false;
	}
	return true;
}

UK_TRACEPOINT(trace_posix_socket_recvfrom, "%d %p %d %d %p %p",
	      int, struct sockaddr *, size_t, int, void *, socklen_t *);
UK_TRACEPOINT(trace_posix_socket_recvfrom_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_recvfrom_err, "%d", int);

UK_SYSCALL_R_DEFINE(ssize_t, recvfrom, int, sock, void *, buf, size_t, len,
		    int, flags, struct sockaddr *, from, socklen_t *, fromlen)
{
	ssize_t ret;
	unsigned int mode;
	struct uk_fdio_wait_context wait;
	struct uk_ofile *of;
	struct sockaddr_storage address = {0};
	socklen_t address_length;

	trace_posix_socket_recvfrom(sock, buf, len, flags, from, fromlen);

	of = socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	mode = of->mode;
	if (mode & O_NONBLOCK)
		flags |= MSG_DONTWAIT;
	wait = uk_fdio_wait_init(of->file, UKFD_POLLIN);
	for (;;) {
		address_length = sizeof(address);
		uk_fdio_wait_snapshot(&wait);
		uk_file_rlock(of->file);
		ret = posix_socket_recvfrom(of->file, buf, len, flags,
			from ? (struct sockaddr *)&address : NULL,
			from ? &address_length : NULL);
		uk_file_runlock(of->file);
		if (!socket_io_retry(&wait, mode, flags, &ret))
			break;
	}
	if (ret >= 0 && from) {
		int error = socket_address_copy(&address, address_length,
						from, fromlen);

		if (error)
			ret = error;
	}
	uk_ofile_release(of);

out:
	if (ret < 0 && ret != -EAGAIN)
		trace_posix_socket_recvfrom_err(ret);
	else
		trace_posix_socket_recvfrom_ret(ret);
	return ret;
}

#if UK_LIBC_SYSCALLS
/* Provide wrapper (if not provided by Musl) or some other libc. */
ssize_t recv(int sock, void *buf, size_t len, int flags)
{
	return uk_syscall_e_recvfrom(sock, (long) buf, len, flags, (long) NULL, (long) NULL);
}
#endif /* UK_LIBC_SYSCALLS */

struct socket_message {
	struct msghdr header;
	void *address_user;
	socklen_t address_capacity;
	struct sockaddr_storage address;
	struct iovec empty;
	struct iovec *vectors;
	void *control;
};

static struct uk_ofile *socket_message_get(struct uk_ofile *of,
					   const struct msghdr *user,
					   struct socket_message *message,
					   bool send)
{
	struct msghdr *header = &message->header;
	size_t metadata, total = 0;
	int error = -EFAULT;

	memset(message, 0, sizeof(*message));
	if (PTRISERR(of))
		return of;
	if (uk_nofault_memcpy((char *)header, (const char *)user,
			      sizeof(*header), 0) != sizeof(*header))
		goto fail;
	if (!send) {
		message->address_user = header->msg_name;
		message->address_capacity = header->msg_namelen;
		header->msg_name =
			message->address_user ? &message->address : NULL;
		header->msg_namelen =
			message->address_user ? sizeof(message->address) : 0;
		header->msg_flags = 0;
	} else {
		if (header->msg_name && header->msg_namelen) {
			if (header->msg_namelen > INT_MAX) {
				error = -EINVAL;
				goto fail;
			}
			header->msg_namelen =
			    MIN(header->msg_namelen, sizeof(message->address));
			if (uk_nofault_memcpy((char *)&message->address,
					      (const char *)header->msg_name,
					      header->msg_namelen,
					      0) != header->msg_namelen)
				goto fail;
			header->msg_name = &message->address;
		} else {
			header->msg_name = NULL;
			header->msg_namelen = 0;
		}
	}
	if (header->msg_iovlen > 1024) {
		error = -EMSGSIZE;
		goto fail;
	}
	if (header->msg_iovlen) {
		metadata = header->msg_iovlen * sizeof(struct iovec);
		message->vectors = uk_malloc(uk_alloc_get_default(), metadata);
		if (!message->vectors) {
			error = -ENOMEM;
			goto fail;
		}
		if (uk_nofault_memcpy((char *)message->vectors,
				      (const char *)header->msg_iov, metadata,
				      0) != metadata)
			goto fail;
		for (size_t index = 0; index < header->msg_iovlen; index++) {
			if (message->vectors[index].iov_len >
			    __SSZ_MAX - total) {
				error = -EINVAL;
				goto fail;
			}
			total += message->vectors[index].iov_len;
		}
		header->msg_iov = message->vectors;
	} else {
		header->msg_iov = &message->empty;
	}
	if (send && header->msg_controllen) {
		if (header->msg_controllen > INT_MAX) {
			error = -ENOBUFS;
			goto fail;
		}
		message->control = uk_malloc(uk_alloc_get_default(),
					     header->msg_controllen);
		if (!message->control) {
			error = -ENOBUFS;
			goto fail;
		}
		if (uk_nofault_memcpy(message->control, header->msg_control,
				      header->msg_controllen,
				      0) != header->msg_controllen)
			goto fail;
		header->msg_control = message->control;
	}
	return of;
fail:
	uk_free(uk_alloc_get_default(), message->control);
	uk_free(uk_alloc_get_default(), message->vectors);
	uk_ofile_release(of);
	return ERR2PTR(error);
}

static void socket_message_put(struct uk_ofile *of,
			       struct socket_message *message)
{
	uk_free(uk_alloc_get_default(), message->control);
	uk_free(uk_alloc_get_default(), message->vectors);
	uk_ofile_release(of);
}

static ssize_t socket_message_io(struct uk_ofile *of, struct msghdr *header,
				 int flags, bool send)
{
	struct uk_fdio_wait_context wait = uk_fdio_wait_init(of->file,
					send ? UKFD_POLLOUT : UKFD_POLLIN);
	ssize_t ret;

	if (!send && (of->mode & O_NONBLOCK))
		flags |= MSG_DONTWAIT;
	for (;;) {
		uk_fdio_wait_snapshot(&wait);
		uk_file_rlock(of->file);
		ret = send ? posix_socket_sendmsg(of->file, header, flags) :
			posix_socket_recvmsg(of->file, header, flags);
		uk_file_runlock(of->file);
		if (!socket_io_retry(&wait, of->mode, flags, &ret))
			break;
	}
	return ret;
}

static ssize_t socket_receive_message(struct uk_ofile *of,
				      struct msghdr *msg, int flags,
				      int *message_flags)
{
	struct socket_message message;
	struct msghdr *header = &message.header;
	ssize_t ret;

	of = socket_message_get(of, msg, &message, false);
	if (PTRISERR(of))
		return PTR2ERR(of);
	ret = socket_message_io(of, header, flags, false);
	if (ret >= 0 && message.address_user) {
		size_t length =
			MIN(message.address_capacity,
			    MIN(header->msg_namelen, sizeof(message.address)));

		if (uk_nofault_memcpy(message.address_user,
				      (const char *)&message.address, length,
				      0) != length)
			ret = -EFAULT;
	}
	if (ret >= 0 &&
	    (!uk_nofault_try_write(header->msg_namelen, &msg->msg_namelen) ||
	     !uk_nofault_try_write(header->msg_controllen,
				   &msg->msg_controllen) ||
	     !uk_nofault_try_write(header->msg_flags, &msg->msg_flags)))
		ret = -EFAULT;
	if (ret >= 0 && message_flags)
		*message_flags = header->msg_flags;
	socket_message_put(of, &message);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_recvmsg, "%d %p %d", int, struct msghdr*, int);
UK_TRACEPOINT(trace_posix_socket_recvmsg_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_recvmsg_err, "%d", int);

UK_SYSCALL_R_DEFINE(ssize_t, recvmsg, int, sock, struct msghdr *, msg,
		    int, flags)
{
	ssize_t ret;

	trace_posix_socket_recvmsg(sock, msg, flags);
	ret = socket_receive_message(socketfd_get(sock), msg, flags, NULL);
	if (ret < 0 && ret != -EAGAIN)
		trace_posix_socket_recvmsg_err(ret);
	else
		trace_posix_socket_recvmsg_ret(ret);
	return ret;
}

#define SOCKET_MSG_CMSG_COMPAT 0x80000000U

UK_SYSCALL_R_DEFINE(int, recvmmsg, int, sock, struct mmsghdr *, messages,
		    unsigned int, count, unsigned int, flags,
		    struct timespec *, timeout)
{
	struct timespec remaining;
	struct uk_ofile *of;
	__nsec deadline = 0;
	unsigned int received = 0;
	ssize_t result = 0;

	if (flags & SOCKET_MSG_CMSG_COMPAT)
		return -EINVAL;
	if (timeout) {
		__nsec now, maximum, seconds, fraction;

		if (!uk_nofault_try_read(remaining, timeout))
			return -EFAULT;
		if (remaining.tv_sec < 0 || remaining.tv_nsec < 0 ||
		    remaining.tv_nsec >= 1000000000)
			return -EINVAL;
		seconds = remaining.tv_sec;
		fraction = remaining.tv_nsec;
		now = ukplat_monotonic_clock();
		maximum = __SNSEC_MAX - now;
		deadline = seconds > maximum / 1000000000 ||
			(seconds == maximum / 1000000000 &&
			 fraction > maximum % 1000000000) ?
			__SNSEC_MAX : now + seconds * 1000000000 + fraction;
	}
	of = socketfd_get(sock);
	if (PTRISERR(of))
		return PTR2ERR(of);
	if (!(flags & MSG_ERRQUEUE)) {
		result = -posix_sock_error(of->file);
		if (result)
			goto out;
	}
	while (received < count) {
		unsigned int length;
		int message_flags;

		uk_ofile_acquire(of);
		result = socket_receive_message(of, &messages[received].msg_hdr,
						flags & ~MSG_WAITFORONE,
						&message_flags);
		if (result < 0)
			break;
		length = result;
		if (!uk_nofault_try_write(length,
					  &messages[received].msg_len)) {
			result = -EFAULT;
			break;
		}
		received++;
		result = 0;
		if (flags & MSG_WAITFORONE)
			flags |= MSG_DONTWAIT;
		if (timeout) {
			__nsec now = ukplat_monotonic_clock();
			__nsec duration = now < deadline ? deadline - now : 0;

			remaining.tv_sec = duration / 1000000000;
			remaining.tv_nsec = duration % 1000000000;
			if (!duration)
				break;
		}
		if (message_flags & MSG_OOB)
			break;
	}
	if (received && result < 0 && result != -EAGAIN)
		posix_sock_error_set(of->file, -result);
	if (received && timeout &&
	    !uk_nofault_try_write(remaining, timeout)) {
		result = -EFAULT;
		received = 0;
	}
out:
	uk_ofile_release(of);
	return received ? (int)received : (int)result;
}

static ssize_t socket_send_result(ssize_t ret, bool write_sigpipe, int flags)
{
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
	if (ret == -EPIPE && write_sigpipe && !(flags & MSG_NOSIGNAL))
		(void)uk_posix_signal_current(SIGPIPE);
#else
	(void)write_sigpipe;
	(void)flags;
#endif
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_sendmsg, "%d %p %d", int,
	      const struct msghdr *, int);
UK_TRACEPOINT(trace_posix_socket_sendmsg_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_sendmsg_err, "%d", int);

UK_SYSCALL_R_DEFINE(ssize_t, sendmsg, int, sock, const struct msghdr *, msg,
		    int, flags)
{
	struct socket_message message;
	ssize_t ret;
	bool write_sigpipe;
	struct uk_ofile *of;

	trace_posix_socket_sendmsg(sock, msg, flags);

	of = socket_message_get(socketfd_get(sock), msg, &message, true);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	ret = socket_message_io(of, &message.header, flags, true);
	write_sigpipe = of->file->write_sigpipe;
	socket_message_put(of, &message);
	ret = socket_send_result(ret, write_sigpipe, flags);

out:
	if (ret < 0 && ret != -EAGAIN)
		trace_posix_socket_sendmsg_err(ret);
	else
		trace_posix_socket_sendmsg_ret(ret);
	return ret;
}

UK_SYSCALL_R_DEFINE(int, sendmmsg, int, sock, struct mmsghdr *, messages,
		    unsigned int, count, unsigned int, flags)
{
	struct uk_ofile *of = socketfd_get(sock);
	unsigned int sent = 0;
	ssize_t result = 0;

	if (PTRISERR(of))
		return PTR2ERR(of);
	count = MIN(count, 1024U);
	while (sent < count) {
		struct socket_message message;
		struct uk_ofile *current;
		unsigned int length;
		bool write_sigpipe;

		uk_ofile_acquire(of);
		current = socket_message_get(of, &messages[sent].msg_hdr,
					     &message, true);
		if (PTRISERR(current)) {
			result = PTR2ERR(current);
			break;
		}
		result = socket_message_io(current, &message.header, flags, true);
		write_sigpipe = current->file->write_sigpipe;
		socket_message_put(current, &message);
		result = socket_send_result(result, write_sigpipe, flags);
		if (result < 0)
			break;
		length = result;
		if (!uk_nofault_try_write(length, &messages[sent].msg_len)) {
			result = -EFAULT;
			break;
		}
		sent++;
	}
	uk_ofile_release(of);
	return sent ? (int)sent : (int)result;
}

UK_TRACEPOINT(trace_posix_socket_sendto, "%d %p %d %d %p %d",
	      int, const void *, size_t, int,
	      const struct sockaddr *, socklen_t);
UK_TRACEPOINT(trace_posix_socket_sendto_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_sendto_err, "%d", int);

UK_SYSCALL_R_DEFINE(ssize_t, sendto, int, sock, const void *, buf, size_t, len,
		    int, flags, const struct sockaddr *, dest_addr, socklen_t,
		    addrlen)
{
	ssize_t ret;
	unsigned int mode;
	bool write_sigpipe;
	struct uk_fdio_wait_context wait;
	struct uk_ofile *of;
	struct sockaddr_storage address = {0};

	trace_posix_socket_sendto(sock, buf, len, flags, dest_addr, addrlen);

	of = dest_addr ? socket_address_get(sock, &address, dest_addr, addrlen)
		       : socketfd_get(sock);
	if (unlikely(PTRISERR(of))) {
		ret = PTR2ERR(of);
		goto out;
	}

	mode = of->mode;
	wait = uk_fdio_wait_init(of->file, UKFD_POLLOUT);
	for (;;) {
		uk_fdio_wait_snapshot(&wait);
		uk_file_rlock(of->file);
		ret = posix_socket_sendto(of->file, buf, len, flags,
		    dest_addr ? (const struct sockaddr *)&address : NULL,
		    dest_addr ? addrlen : 0);
		uk_file_runlock(of->file);
		if (!socket_io_retry(&wait, mode, flags, &ret))
			break;
	}
	write_sigpipe = of->file->write_sigpipe;
	uk_ofile_release(of);
	ret = socket_send_result(ret, write_sigpipe, flags);

out:
	if (ret < 0 && ret != -EAGAIN)
		trace_posix_socket_sendto_err(ret);
	else
		trace_posix_socket_sendto_ret(ret);
	return ret;
}

#if UK_LIBC_SYSCALLS
/* Provide wrapper (if not provided by Musl) or some other libc. */
ssize_t send(int sock, const void *buf, size_t len, int flags)
{
	return uk_syscall_e_sendto(sock, (long)buf, len, flags, (long)NULL, 0);
}
#endif /* UK_LIBC_SYSCALLS */

#endif /* CONFIG_LIBPOSIX_FDTAB */


int uk_socketpair_create(int family, int type, int protocol,
			 const struct uk_file *sv[2])
{
	int ret;
	struct socket_alloc *al[2];
	void *sock_data[2];
	struct posix_socket_driver *d = posix_socket_driver_get(family);

	if (unlikely(!d))
		return -EAFNOSUPPORT;

	al[0] = uk_malloc(d->allocator, sizeof(struct socket_alloc));
	al[1] = uk_malloc(d->allocator, sizeof(struct socket_alloc));
	if (unlikely(!al[0] || !al[1])) {
		ret = -ENOMEM;
		goto err_free;
	}

	ret = posix_socket_socketpair(d, family, type, protocol, sock_data);
	if (unlikely(ret))
		goto err_free;

	_socket_init(al[0], d, sock_data[0],
		     (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_STREAM);
	uk_socket_evd_init(&al[0]->evd, family, type, protocol);
	_socket_init(al[1], d, sock_data[1],
		     (type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) == SOCK_STREAM);
	uk_socket_evd_init(&al[1]->evd, family, type, protocol);
	sv[0] = &al[0]->f;
	sv[1] = &al[1]->f;
	posix_socket_socketpair_post(d, sv);
	/* NOTE: If we fail later in `uk_sys_socketpair()`, release calls
	 *       during error cleanup will cause raising `CLOSE` events
	 *       immediately.
	 */
	uk_socket_event_raise(&al[0]->evd, CONNECT);
	uk_socket_event_raise(&al[1]->evd, CONNECT);
	return 0;

err_free:
	uk_free(d->allocator, al[0]);
	uk_free(d->allocator, al[1]);
	return ret;
}

#if CONFIG_LIBPOSIX_FDTAB
int uk_sys_socketpair(int family, int type, int protocol, int sv[2])
{
	int ret;
	const struct uk_file *socks[2];
	unsigned int mode = SOCKET_MODE;

	ret = uk_socketpair_create(family, type, protocol, socks);
	if (unlikely(ret))
		return ret;

	if (type & SOCK_NONBLOCK)
		mode |= O_NONBLOCK;
	if (type & SOCK_CLOEXEC)
		mode |= O_CLOEXEC;

	ret = uk_fdtab_open_named(socks[0], mode, POSIX_SOCKET_PAIR_FNAME,
				  POSIX_SOCKET_PAIR_FNAME_LEN);
	if (unlikely(ret < 0))
		goto out;
	sv[0] = ret;

	ret = uk_fdtab_open_named(socks[1], mode, POSIX_SOCKET_PAIR_FNAME,
				  POSIX_SOCKET_PAIR_FNAME_LEN);
	if (unlikely(ret < 0)) {
		uk_sys_close(sv[0]);
		goto out;
	}
	sv[1] = ret;
	ret = 0;

out:
	uk_file_release(socks[0]);
	uk_file_release(socks[1]);
	return ret;
}

UK_TRACEPOINT(trace_posix_socket_socketpair, "%d %d %d %p", int, int, int,
	      int *);
UK_TRACEPOINT(trace_posix_socket_socketpair_ret, "%d", int);
UK_TRACEPOINT(trace_posix_socket_socketpair_err, "%d", int);

UK_SYSCALL_R_DEFINE(int, socketpair, int, family, int, type, int, protocol,
		    int *, usockfd)
{
	int ret;

	trace_posix_socket_socketpair(family, type, protocol, usockfd);

	if (unlikely(!usockfd))
		return -EFAULT;

	ret = uk_sys_socketpair(family, type, protocol, usockfd);

	if (ret)
		trace_posix_socket_socketpair_err(ret);
	else
		trace_posix_socket_socketpair_ret(ret);
	return ret;
}
#endif /* CONFIG_LIBPOSIX_FDTAB */
