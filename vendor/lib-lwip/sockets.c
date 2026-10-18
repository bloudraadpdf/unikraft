/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Alexander Jung <alexander.jung@neclab.eu>
 *          Marc Rittinghaus <marc.rittinghaus@kit.edu>
 *
 * Copyright (c) 2020, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2021, Karlsruhe Institute of Technology (KIT).
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

#include <stdint.h>
#include <string.h>

#include <uk/config.h>
#include <uk/alloc.h>
#include <uk/mutex.h>
#include <uk/atomic.h>
#include <uk/assert.h>
#include <uk/print.h>
#include <uk/essentials.h>
#include <uk/socket_driver.h>
#include <uk/nofault.h>

#include <lwip/sockets.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <lwip/priv/sockets_priv.h>
#include <lwip/api.h>
#include <lwip/sys.h>
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/tcpbase.h>
#include <lwip/err.h>
#include <lwip/priv/tcpip_priv.h>
#include <net/if.h>

static void lwip_posix_socket_poll_setup(posix_sock *file);

struct lwip_socket_data {
	struct uk_alloc *allocator;
	struct uk_mutex receive;
	struct uk_mutex send;
	int fd;
};

static void *lwip_socket_data_create(struct uk_alloc *allocator, int fd)
{
	struct lwip_socket_data *data = uk_malloc(allocator, sizeof(*data));

	if (!data) {
		lwip_close(fd);
		return ERR2PTR(-ENOMEM);
	}
	data->allocator = allocator;
	data->fd = fd;
	uk_mutex_init(&data->receive);
	uk_mutex_init(&data->send);
	return data;
}

static inline
int _lwip_ignore_noncrit_msgflags(int flags)
{
	return flags & ~(MSG_NOSIGNAL);
}

static inline
int _lwip_getfd(posix_sock *sock)
{
	struct lwip_socket_data *data = posix_sock_get_data(sock);

	return data->fd;
}

static int
lwip_socket_apply_flags(int lwip_fd, int flags)
{
	int val = 1;

	/* Set FIONBIO unconditionally, as posix-socket handles blocking
	 * internally and expects lwip sockets to be always non-blocking.
	 */
	val = lwip_ioctl(lwip_fd, FIONBIO, &val);
	if (unlikely(val < 0)) {
		return -errno;
	}

	/* Ignore SOCK_CLOEXEC */

	return 0;
}

static void *
lwip_posix_socket_create(struct posix_socket_driver *d, int family, int type,
			 int protocol)
{
	int lwip_fd;
	int flags, rc;

	flags = type & SOCK_FLAGS;
	type = type & ~SOCK_FLAGS;

	lwip_fd = lwip_socket(family, type, protocol);
	if (unlikely(lwip_fd < 0))
		return ERR2PTR(-errno);

	rc = lwip_socket_apply_flags(lwip_fd, flags);
	if (unlikely(rc)) {
		(void)lwip_close(lwip_fd);
		return ERR2PTR(rc);
	}

	return lwip_socket_data_create(d->allocator, lwip_fd);
}

static void *
lwip_posix_socket_accept4(posix_sock *file,
			  struct sockaddr *restrict addr,
			  socklen_t *restrict addr_len, int flags)
{
	int listen_fd, new_fd;
	int rc;

	listen_fd = _lwip_getfd(file);
	UK_ASSERT(listen_fd >= 0);

	new_fd = lwip_accept(listen_fd, addr, addr_len);
	if (unlikely(new_fd < 0))
		return ERR2PTR(-errno);

	rc = lwip_socket_apply_flags(new_fd, flags);
	if (unlikely(rc)) {
		(void)lwip_close(new_fd);
		return ERR2PTR(rc);
	}

	return lwip_socket_data_create(posix_sock_get_driver(file)->allocator,
				       new_fd);
}

static int
lwip_posix_socket_bind(posix_sock *file,
		       const struct sockaddr *addr,
		       socklen_t addr_len)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_bind(lwip_fd, addr, addr_len);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static int
lwip_posix_socket_shutdown(posix_sock *file, int how)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_shutdown(lwip_fd, how);
	if (unlikely(ret < 0))
		ret = -errno;
	lwip_posix_socket_poll_setup(file);
	if (!ret && how != SHUT_WR)
		posix_sock_event_set(file, EPOLLIN | EPOLLRDNORM | EPOLLRDHUP |
				     (how == SHUT_RDWR ? EPOLLHUP : 0));

	return ret;
}

static int
lwip_posix_socket_getpeername(posix_sock *file,
			      struct sockaddr *restrict addr,
			      socklen_t *restrict addr_len)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_getpeername(lwip_fd, addr, addr_len);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static int
lwip_posix_socket_getsockname(posix_sock *file,
			      struct sockaddr *restrict addr,
			      socklen_t *restrict addr_len)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_getsockname(lwip_fd, addr, addr_len);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static int
lwip_posix_socket_getsockopt(posix_sock *file, int level,
			     int optname, void *restrict optval,
			     socklen_t *restrict optlen)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_getsockopt(lwip_fd, level, optname, optval, optlen);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

#define LINUX_SOL_TCP 6
#define LINUX_TCP_FASTOPEN 23

static int
lwip_posix_socket_setsockopt(posix_sock *file, int level,
			     int optname, const void *optval, socklen_t optlen)
{
	int lwip_fd;
	int ret;
	int ip_value = 0;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	if (level == IPPROTO_IP &&
	    (optname == IP_TTL || optname == IP_TOS || optname == IP_RECVERR)) {
		if (optlen >= sizeof(ip_value)) {
			if (!uk_nofault_try_read(ip_value, optval))
				return -EFAULT;
		} else if (optlen) {
			unsigned char byte;

			if (!uk_nofault_try_read(byte, optval))
				return -EFAULT;
			ip_value = byte;
		} else if (optname == IP_TTL) {
			return -EINVAL;
		}
		optval = &ip_value;
		optlen = sizeof(ip_value);
	}
	if ((level == LINUX_SOL_TCP && optname == LINUX_TCP_FASTOPEN) ||
	    (level == SOL_IP && optname == IP_RECVERR)) {
		int value;

		if (optlen < sizeof(value))
			return -EINVAL;
		if (!uk_nofault_try_read(value, optval))
			return -EFAULT;
		return 0;
	}
	ret = lwip_setsockopt(lwip_fd, level, optname, optval, optlen);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static int
lwip_posix_socket_connect(posix_sock *file,
			  const struct sockaddr *addr, socklen_t addr_len)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_connect(lwip_fd, addr, addr_len);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static int
lwip_posix_socket_listen(posix_sock *file, int backlog)
{
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_listen(lwip_fd, backlog);
	if (unlikely(ret < 0))
		ret = -errno;

	return ret;
}

static struct lwip_socket_data *
lwip_socket_io_begin(posix_sock *file, bool write)
{
	struct lwip_socket_data *data = posix_sock_get_data(file);

	UK_ASSERT(data->fd >= 0);
	uk_mutex_lock(write ? &data->send : &data->receive);
	return data;
}

static void lwip_receive_state_publish(posix_sock *file,
				       struct lwip_socket_data *data)
{
	struct lwip_sock *sock = lwip_socket_dbg_get_socket(data->fd);

	uk_store_n(&sock->pending_read, !!sock->lastdata.pointer);
	lwip_posix_socket_poll_setup(file);
}

static ssize_t
lwip_socket_io_end(posix_sock *file, struct lwip_socket_data *data,
		   ssize_t result, bool write)
{
	if (unlikely(result < 0))
		result = -errno;
	if (!write)
		lwip_receive_state_publish(file, data);
	uk_mutex_unlock(write ? &data->send : &data->receive);
	return result;
}

static ssize_t
lwip_posix_socket_recvfrom(posix_sock *file, void *restrict buf,
			   size_t len, int flags, struct sockaddr *from,
			   socklen_t *restrict fromlen)
{
	struct lwip_socket_data *data;

	flags = _lwip_ignore_noncrit_msgflags(flags);
	data = lwip_socket_io_begin(file, false);
	return lwip_socket_io_end(file, data,
		lwip_recvfrom(data->fd, buf, len, flags, from, fromlen), false);
}

static ssize_t
lwip_posix_socket_recvmsg(posix_sock *file, struct msghdr *msg,
			  int flags)
{
	struct lwip_socket_data *data;

	flags = _lwip_ignore_noncrit_msgflags(flags);
	data = lwip_socket_io_begin(file, false);
	return lwip_socket_io_end(file, data,
		lwip_recvmsg(data->fd, msg, flags), false);
}

static ssize_t
lwip_posix_socket_sendmsg(posix_sock *file,
			  const struct msghdr *msg, int flags)
{
	struct lwip_socket_data *data;

	flags = _lwip_ignore_noncrit_msgflags(flags);
	data = lwip_socket_io_begin(file, true);
	return lwip_socket_io_end(file, data,
		lwip_sendmsg(data->fd, msg, flags), true);
}

static ssize_t
lwip_posix_socket_sendto(posix_sock *file, const void *buf,
			 size_t len, int flags,
			 const struct sockaddr *dest_addr,
			 socklen_t addrlen)
{
	struct lwip_socket_data *data;

	flags = _lwip_ignore_noncrit_msgflags(flags);
	data = lwip_socket_io_begin(file, true);
	return lwip_socket_io_end(file, data,
		lwip_sendto(data->fd, buf, len, flags,
			    dest_addr, addrlen), true);
}

static ssize_t lwip_posix_socket_read(posix_sock *file, const struct iovec *iov,
				      size_t iovcnt, int flags)
{
	struct lwip_socket_data *data;
	struct msghdr message = {
		.msg_iov = (struct iovec *)iov,
		.msg_iovlen = iovcnt,
	};

	data = lwip_socket_io_begin(file, false);
	return lwip_socket_io_end(file, data,
				  lwip_recvmsg(data->fd, &message, flags),
				  false);
}

static ssize_t
lwip_posix_socket_write(posix_sock *file, const struct iovec *iov,
			size_t iovcnt)
{
	struct lwip_socket_data *data;

	data = lwip_socket_io_begin(file, true);
	return lwip_socket_io_end(file, data,
		lwip_writev(data->fd, iov, iovcnt), true);
}

static int
lwip_posix_socket_close(posix_sock *file)
{
	struct lwip_socket_data *data = posix_sock_get_data(file);
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	ret = lwip_close(lwip_fd);
	if (unlikely(ret < 0))
		ret = -errno;

	uk_free(data->allocator, data);
	return ret;
}

struct lwip_interface_query {
	struct tcpip_api_call_data call;
	struct ifreq request;
	int command, result;
};

static err_t lwip_interface_query(struct tcpip_api_call_data *call)
{
	struct lwip_interface_query *query =
		__containerof(call, struct lwip_interface_query, call);
	struct ifreq *request = &query->request;
	struct netif *interface;

	query->result = -ENODEV;
	if (query->command == SIOCGIFNAME) {
		if (request->ifr_ifindex <= 0 ||
		    request->ifr_ifindex > (int)UINT8_MAX)
			return ERR_OK;
		interface = netif_get_by_index((u8_t)request->ifr_ifindex);
		if (interface) {
			memset(request->ifr_name, 0, sizeof(request->ifr_name));
			netif_index_to_name(netif_get_index(interface),
					    request->ifr_name);
			query->result = 0;
		}
	} else {
		request->ifr_name[sizeof(request->ifr_name) - 1] = '\0';
		NETIF_FOREACH(interface) {
			char name[NETIF_NAMESIZE];

			netif_index_to_name(netif_get_index(interface), name);
			if (strcmp(request->ifr_name, name))
				continue;
			request->ifr_ifindex = netif_get_index(interface);
			query->result = 0;
			break;
		}
	}
	return ERR_OK;
}

static int lwip_interface_ioctl(int command, void *user)
{
	struct lwip_interface_query query = {.command = command};
	err_t error;

	if (!uk_nofault_try_read(query.request, (const struct ifreq *)user))
		return -EFAULT;
	error = tcpip_api_call(lwip_interface_query, &query.call);
	if (error != ERR_OK)
		return -err_to_errno(error);
	if (query.result)
		return query.result;
	return uk_nofault_try_write(query.request, (struct ifreq *)user)
		       ? 0
		       : -EFAULT;
}

static int
lwip_posix_socket_ioctl(posix_sock *file, int request, void *argp)
{
	struct lwip_socket_data *data = posix_sock_get_data(file);
	int lwip_fd;
	int ret;

	lwip_fd = _lwip_getfd(file);
	UK_ASSERT(lwip_fd >= 0);

	uk_mutex_lock(&data->receive);
	ret = lwip_ioctl(lwip_fd, request, argp);
	if (unlikely(ret < 0))
		ret = -errno;
	lwip_receive_state_publish(file, data);

	uk_mutex_unlock(&data->receive);
	return ret;
}

#if LWIP_NETCONN_FULLDUPLEX
#define NETCONN_RECVMBOX_WAITABLE(conn)					\
	(sys_mbox_valid(&(conn)->recvmbox) &&				\
	 (((conn)->flags & NETCONN_FLAG_MBOXINVALID) == 0))
#else /* LWIP_NETCONN_FULLDUPLEX */
#define NETCONN_RECVMBOX_WAITABLE(conn)					\
	sys_mbox_valid(&(conn)->recvmbox)
#endif /* LWIP_NETCONN_FULLDUPLEX */

static unsigned int
get_lwip_socket_events(struct lwip_sock *sock)
{
	unsigned int events = 0;

	UK_ASSERT(sock);

#if LWIP_TCP
	if ((NETCONNTYPE_GROUP(sock->conn->type) == NETCONN_TCP)) {
		/* A TCP connection may be in not-connected state. Don't report it as
		 * readable or writeable.
		 */
		if ((sock->conn->state == NETCONN_NONE) &&
		    (!NETCONN_RECVMBOX_WAITABLE(sock->conn))) {
			if (sock->errevent != 0)
				events |= EPOLLERR;

			return events;
		}

                /* Check whether the TCP connection is in CLOSE_WAIT (= got a
                 * FIN packet). In that case the peer will not sent more data
                 * and we can tell the application that the receive buffer is
                 * everything we got until the EOF.
                 */
                if (sock->conn->pcb.tcp == NULL ||
		    sock->conn->pcb.tcp->state == CLOSE_WAIT)
			events |= EPOLLRDHUP;
	}

#endif

	if (NETCONNTYPE_GROUP(sock->conn->type) == NETCONN_UDP) {
		unsigned int shutdown = uk_load_n(&sock->shutdown);

		if (shutdown & LWIP_SOCKET_SHUT_RD)
			events |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;
		if (shutdown == (LWIP_SOCKET_SHUT_RD | LWIP_SOCKET_SHUT_WR))
			events |= EPOLLHUP;
	}

	if (uk_load_n(&sock->pending_read) || sock->rcvevent > 0)
		events |= EPOLLIN | EPOLLRDNORM;
	if (sock->sendevent != 0)
		events |= EPOLLOUT | EPOLLWRNORM;
	if (sock->errevent != 0)
		events |= EPOLLERR;

	return events;
}

void
lwip_posix_socket_event_callback(struct lwip_sock *sock,
				 enum netconn_evt evt __unused,
				 u16_t len __unused)
{
	posix_sock *sockobj;
	unsigned int events;

	UK_ASSERT(sock);
	if (unlikely(!sock->sock_data))
		return;

	sockobj = (posix_sock *)sock->sock_data;
	UK_ASSERT(_lwip_getfd(sockobj) == sock->conn->socket);

	events = get_lwip_socket_events(sock);
	posix_sock_event_assign(sockobj, events);
}

struct lwip_socket_state_query {
	struct tcpip_api_call_data call;
	posix_sock *file;
	enum posix_socket_timeout direction;
	__u64 milliseconds;
	bool set_timeout;
};

static err_t lwip_socket_state_query(struct tcpip_api_call_data *call)
{
	struct lwip_socket_state_query *query =
		__containerof(call, struct lwip_socket_state_query, call);
	posix_sock *file = query->file;
	struct lwip_sock *sock;
	SYS_ARCH_DECL_PROTECT(lev);

	SYS_ARCH_PROTECT(lev);
	sock = lwip_socket_dbg_get_socket(_lwip_getfd(file));
	if (query->set_timeout) {
		if (query->direction == POSIX_SOCKET_TIMEOUT_RECEIVE)
			sock->conn->socket_receive_timeout_ms =
				query->milliseconds;
		else
			sock->conn->socket_send_timeout_ms =
				query->milliseconds;
	}
	sock->sock_data = (void *)file;
	posix_sock_get_node(file)->receive_timeout_ms =
		sock->conn->socket_receive_timeout_ms;
	posix_sock_get_node(file)->send_timeout_ms =
		sock->conn->socket_send_timeout_ms;
	posix_sock_event_assign(file, get_lwip_socket_events(sock));
	SYS_ARCH_UNPROTECT(lev);
	return ERR_OK;
}

static int
lwip_posix_socket_timeout_update(posix_sock *file,
				 enum posix_socket_timeout direction,
				 __u64 milliseconds)
{
	struct lwip_socket_state_query query = {
		.file = file,
		.direction = direction,
		.milliseconds = milliseconds,
		.set_timeout = true,
	};

	return -err_to_errno(
		tcpip_api_call(lwip_socket_state_query, &query.call));
}

static void
lwip_posix_socket_poll_setup(posix_sock *file)
{
	struct lwip_socket_state_query query = {.file = file};

	if (tcpip_api_call(lwip_socket_state_query, &query.call) != ERR_OK)
		posix_sock_event_set(file, EPOLLERR);
}

static struct posix_socket_ops lwip_posix_socket_ops = {
	.nofault_io = true,
	.interface_ioctl = lwip_interface_ioctl,
	/* POSIX interfaces */
	.create		= lwip_posix_socket_create,
	.accept4	= lwip_posix_socket_accept4,
	.bind		= lwip_posix_socket_bind,
	.shutdown	= lwip_posix_socket_shutdown,
	.getpeername	= lwip_posix_socket_getpeername,
	.getsockname	= lwip_posix_socket_getsockname,
	.getsockopt	= lwip_posix_socket_getsockopt,
	.setsockopt	= lwip_posix_socket_setsockopt,
	.connect	= lwip_posix_socket_connect,
	.listen		= lwip_posix_socket_listen,
	.recvfrom	= lwip_posix_socket_recvfrom,
	.recvmsg	= lwip_posix_socket_recvmsg,
	.sendmsg	= lwip_posix_socket_sendmsg,
	.sendto		= lwip_posix_socket_sendto,
	/* vfscore ops */
	.read		= lwip_posix_socket_read,
	.write		= lwip_posix_socket_write,
	.close		= lwip_posix_socket_close,
	.ioctl		= lwip_posix_socket_ioctl,
	.poll_setup	= lwip_posix_socket_poll_setup,
	.timeout_update = lwip_posix_socket_timeout_update,
};

POSIX_SOCKET_FAMILY_REGISTER(AF_INET, &lwip_posix_socket_ops);

#ifdef CONFIG_LWIP_IPV6
POSIX_SOCKET_FAMILY_REGISTER(AF_INET6, &lwip_posix_socket_ops);
#endif /* CONFIG_LWIP_IPV6 */
