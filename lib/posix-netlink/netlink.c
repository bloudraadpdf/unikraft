/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <uk/nofault.h>

#include <uk/file/iovutil.h>
#include <uk/netlink/driver.h>
#include <uk/socket_driver.h>
#include <uk/print.h>

#if CONFIG_LIBPOSIX_PROCESS
#include <uk/process.h>
#endif /* CONFIG_LIBPOSIX_PROCESS */

#define NL_CONNECTED 1
#define NL_BOUND 2

#define sock2nlctx(s) ((struct nl_ctx *)posix_sock_get_data(s))

extern struct posix_netlink_protocol posix_netlink_protocol_list_start[];
extern struct posix_netlink_protocol posix_netlink_protocol_list_end[];

static const struct posix_netlink_protocol *nl_driver_find(int protocol)
{
	const struct posix_netlink_protocol *drv;

	drv = posix_netlink_protocol_list_start;
	while (drv != posix_netlink_protocol_list_end) {
		if (drv->protocol == protocol)
			return drv;
		drv++;
	}
	return NULL;
}

#define NL_RECV_MAX_MESSAGES 512
#define NL_RECV_MAX_BYTES (256 * 1024)

struct nl_message {
	struct uk_list_head link;
	struct uk_streambuf *buffer;
	size_t charge;
};

static UK_LIST_HEAD(nl_ports);
static struct uk_mutex nl_ports_lock = UK_MUTEX_INITIALIZER(nl_ports_lock);
static __u32 nl_next_port = 0x80000000U;

static void nl_batch_init(struct nl_reply_batch *batch, struct nl_ctx *context)
{
	*batch = (struct nl_reply_batch){.context = context};
	UK_INIT_LIST_HEAD(&batch->messages);
}

static void nl_message_free(struct nl_ctx *context, struct nl_message *message)
{
	nlbuf_free(message->buffer);
	uk_free(context->allocator, message);
}

static void nl_batch_discard(struct nl_reply_batch *batch)
{
	struct nl_message *message, *next;

	uk_list_for_each_entry_safe(message, next, &batch->messages, link) {
		uk_list_del(&message->link);
		nl_message_free(batch->context, message);
	}
	batch->bytes = 0;
	batch->count = 0;
}

int nlbuf_send(struct nl_reply_batch *batch, struct uk_streambuf *buffer)
{
	struct nl_message *message;
	size_t overhead = sizeof(*message) + sizeof(*buffer);
	size_t charge;
	int error;

	if (buffer->buflen > NL_RECV_MAX_BYTES - overhead) {
		error = -ENOBUFS;
		goto fail;
	}
	charge = overhead + buffer->buflen;
	if (batch->count == NL_RECV_MAX_MESSAGES ||
	    charge > NL_RECV_MAX_BYTES - batch->bytes) {
		error = -ENOBUFS;
		goto fail;
	}
	message = uk_malloc(batch->context->allocator, sizeof(*message));
	if (!message) {
		error = -ENOMEM;
		goto fail;
	}
	message->buffer = buffer;
	message->charge = charge;
	uk_list_add_tail(&message->link, &batch->messages);
	batch->bytes += charge;
	batch->count++;
	return 0;
fail:
	nlbuf_free(buffer);
	return error;
}

static bool nl_port_used(const struct nl_ctx *context, __u32 port)
{
	struct nl_ctx *other;

	uk_list_for_each_entry(other, &nl_ports, port_link)
		if (other->drv->protocol == context->drv->protocol &&
		    nl_ctx_pid(other) == port)
			return true;
	return false;
}

static int nl_port_bind(struct nl_ctx *context, __u32 port, __u32 groups,
			bool automatic)
{
	__u32 first;
	int result = 0;
#if CONFIG_LIBPOSIX_PROCESS
	pid_t pid;
#endif

	uk_mutex_lock(&nl_ports_lock);
	if (uk_load_n(&context->flags) & NL_BOUND) {
		result = automatic ? 0 : -EINVAL;
		goto out;
	}
	if (port && nl_port_used(context, port)) {
		result = -EADDRINUSE;
		goto out;
	}
	if (!port) {
#if CONFIG_LIBPOSIX_PROCESS
		pid = uk_sys_getpid();
		port = pid > 0 ? (__u32)pid : 0;
#else
		port = 1;
#endif
		if (!port || nl_port_used(context, port)) {
			first = nl_next_port;
			do {
				port = nl_next_port;
				nl_next_port =
					port == 1 ? 0xffffffffU : port - 1;
				if (!nl_port_used(context, port))
					break;
			} while (nl_next_port != first);
			if (nl_port_used(context, port)) {
				result = -EADDRINUSE;
				goto out;
			}
		}
	}
	uk_store_n(&context->nl_pid, port);
	uk_store_n(&context->nl_groups, groups);
	uk_or(&context->flags, NL_BOUND);
	uk_list_add_tail(&context->port_link, &nl_ports);
out:
	uk_mutex_unlock(&nl_ports_lock);
	return result;
}

static void *nl_create(struct posix_socket_driver *driver,
		       int family __maybe_unused, int type, int protocol)
{
	const struct posix_netlink_protocol *protocol_driver;
	struct nl_ctx *context;
	int error;

	UK_ASSERT(family == AF_NETLINK);
	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (type != SOCK_RAW && type != SOCK_DGRAM)
		return ERR2PTR(-ESOCKTNOSUPPORT);
	protocol_driver = nl_driver_find(protocol);
	if (!protocol_driver)
		return ERR2PTR(-EPROTONOSUPPORT);
	context = uk_zalloc(driver->allocator, sizeof(*context));
	if (!context)
		return ERR2PTR(-ENOMEM);
	context->allocator = driver->allocator;
	context->drv = protocol_driver;
	uk_mutex_init(&context->receive_lock);
	nl_batch_init(&context->receive, context);
	UK_INIT_LIST_HEAD(&context->port_link);
	if (protocol_driver->ops->create) {
		error = protocol_driver->ops->create(context);
		if (error) {
			uk_free(context->allocator, context);
			return ERR2PTR(error);
		}
	}
	return context;
}

static int nl_close(posix_sock *socket)
{
	struct nl_ctx *context = sock2nlctx(socket);

	if (context->drv->ops->close)
		context->drv->ops->close(context);
	uk_mutex_lock(&nl_ports_lock);
	uk_list_del(&context->port_link);
	uk_mutex_unlock(&nl_ports_lock);
	nl_batch_discard(&context->receive);
	uk_free(context->allocator, context);
	return 0;
}

static int nl_bind(posix_sock *socket, const struct sockaddr *address,
		   socklen_t length)
{
	const struct sockaddr_nl *netlink = (const struct sockaddr_nl *)address;

	if (!address || length < sizeof(*netlink))
		return -EINVAL;
	if (netlink->nl_family != AF_NETLINK)
		return -EINVAL;
	if (netlink->nl_groups)
		return -EOPNOTSUPP;
	return nl_port_bind(sock2nlctx(socket), netlink->nl_pid, 0, false);
}

static int nl_sockname(posix_sock *s, struct sockaddr *restrict addr,
		       socklen_t *restrict addr_len)
{
	struct nl_ctx *nl_ctx = sock2nlctx(s);
	const struct sockaddr_nl nl_addr = {
		.nl_family = AF_NETLINK,
		.nl_pad = 0,
		.nl_pid = nl_ctx_pid(nl_ctx),
		.nl_groups = uk_load_n(&nl_ctx->nl_groups)};

	if (unlikely(!addr || !addr_len))
		return -EFAULT;

	memcpy(addr, &nl_addr, MIN(*addr_len, sizeof(nl_addr)));
	*addr_len = sizeof(nl_addr);
	return 0;
}

static int nl_connect(posix_sock *s, const struct sockaddr *addr,
		      socklen_t addr_len)
{
	struct nl_ctx *nl_ctx = sock2nlctx(s);
	const struct sockaddr_nl *nl_addr;

	/* We can only ever send messages to the kernel, thus connect only
	 * validates its args and returns success.
	 */

	if (unlikely(!addr || addr_len < sizeof(*nl_addr)))
		return -EINVAL;

	nl_addr = (const struct sockaddr_nl *)addr;
	if (unlikely(nl_addr->nl_family != AF_NETLINK))
		return -EAFNOSUPPORT;
	if (unlikely(nl_addr->nl_pid != 0))
		return -EHOSTUNREACH;
	int result = nl_port_bind(nl_ctx, 0, 0, true);

	if (!result)
		uk_or(&nl_ctx->flags, NL_CONNECTED);
	return result;
}

static int nl_getpeername(posix_sock *s, struct sockaddr *restrict addr,
			  socklen_t *restrict addr_len)
{
	struct nl_ctx *nl_ctx = sock2nlctx(s);
	const struct sockaddr_nl nl_addr = {
		.nl_family = AF_NETLINK,
		.nl_pad = 0,
		.nl_pid = 0,   /* Only kernel can be peer */
		.nl_groups = 0 /* All messages are unicast */
	};

	if (unlikely(!addr || !addr_len))
		return -EFAULT;
	if (unlikely(!(uk_load_n(&nl_ctx->flags) & NL_CONNECTED)))
		return -ENOTCONN;
	/* Write out peer (truncating if needed) */
	memcpy(addr, &nl_addr, MIN(*addr_len, sizeof(nl_addr)));
	*addr_len = sizeof(nl_addr);
	return 0;
}

static int nl_batch_commit(struct nl_reply_batch *batch)
{
	struct nl_ctx *context = batch->context;
	int result = 0;

	uk_mutex_lock(&context->receive_lock);
	if (batch->count > NL_RECV_MAX_MESSAGES - context->receive.count ||
	    batch->bytes > NL_RECV_MAX_BYTES - context->receive.bytes) {
		result = -ENOBUFS;
	} else {
		uk_list_splice_tail_init(&batch->messages,
					 &context->receive.messages);
		context->receive.bytes += batch->bytes;
		context->receive.count += batch->count;
		batch->bytes = 0;
		batch->count = 0;
		if (context->receive.count)
			posix_sock_event_set(context->socket, EPOLLIN);
	}
	uk_mutex_unlock(&context->receive_lock);
	return result;
}

static int nl_handle(struct nl_reply_batch *batch, const void *buffer,
		     size_t length)
{
	const struct nlmsghdr *header = buffer;
	struct nl_ctx *context = batch->context;
	size_t aligned;
	int result;

	if (length < sizeof(*header))
		return -EINVAL;
	while (length >= sizeof(*header) &&
	       header->nlmsg_len >= sizeof(*header) &&
	       header->nlmsg_len <= length) {
		result = context->drv->ops->handle(context, batch, header);
		if (result)
			return result;
		aligned = NLMSG_ALIGN((size_t)header->nlmsg_len);
		if (aligned > length)
			break;
		length -= aligned;
		header = (const void *)((const char *)header + aligned);
	}
	return 0;
}

static ssize_t nl_sendv(posix_sock *socket, const struct iovec *vectors,
			size_t count, int flags, const struct sockaddr *address,
			socklen_t address_length)
{
	struct nl_ctx *context = sock2nlctx(socket);
	const struct sockaddr_nl *destination = (const void *)address;
	struct nl_reply_batch batch;
	size_t length = 0, offset = 0;
	void *buffer;
	int result;

	if (flags & MSG_OOB)
		return -EOPNOTSUPP;
	if (address) {
		if (address_length < sizeof(*destination))
			return -EINVAL;
		if (destination->nl_family != AF_NETLINK)
			return -EAFNOSUPPORT;
		if (destination->nl_pid)
			return -EHOSTUNREACH;
		if (destination->nl_groups)
			return -EOPNOTSUPP;
	}
	for (size_t i = 0; i < count; i++) {
		if (vectors[i].iov_len > INT_MAX - length)
			return -EMSGSIZE;
		length += vectors[i].iov_len;
	}
	if (!length)
		return -ENODATA;
	buffer = uk_malloc(context->allocator, length);
	if (!buffer)
		return -ENOMEM;
	for (size_t i = 0; i < count; i++) {
		size_t bytes = vectors[i].iov_len;

		if (bytes &&
		    uk_nofault_memcpy((char *)buffer + offset,
				      vectors[i].iov_base, bytes, 0) != bytes) {
			result = -EFAULT;
			goto free_buffer;
		}
		offset += bytes;
	}
	result = nl_port_bind(context, 0, 0, true);
	if (result)
		goto free_buffer;
	nl_batch_init(&batch, context);
	result = nl_handle(&batch, buffer, length);
	if (!result)
		result = nl_batch_commit(&batch);
	nl_batch_discard(&batch);
free_buffer:
	uk_free(context->allocator, buffer);
	return result ? result : (ssize_t)length;
}

static ssize_t nl_sendto(posix_sock *socket, const void *buffer, size_t length,
			 int flags, const struct sockaddr *address,
			 socklen_t address_length)
{
	const struct iovec vector = {(void *)buffer, length};

	return nl_sendv(socket, &vector, 1, flags, address, address_length);
}

static ssize_t nl_recvmsg(posix_sock *socket, struct msghdr *message, int flags)
{
	struct nl_ctx *context = sock2nlctx(socket);
	struct nl_message *packet;
	struct sockaddr_nl address = {.nl_family = AF_NETLINK};
	size_t length, copied = 0;
	ssize_t result;

	if (flags & MSG_OOB)
		return -EOPNOTSUPP;
	uk_mutex_lock(&context->receive_lock);
	packet = uk_list_first_entry_or_null(&context->receive.messages,
					     struct nl_message, link);
	if (!packet) {
		result = -EAGAIN;
		goto out;
	}
	length = nlbuf_len(packet->buffer);
	for (size_t i = 0; i < message->msg_iovlen && copied < length; i++) {
		size_t bytes =
			MIN(message->msg_iov[i].iov_len, length - copied);

		if (bytes &&
		    uk_nofault_memcpy(message->msg_iov[i].iov_base,
				      (const char *)nlbuf_data(packet->buffer) +
					      copied,
				      bytes, 0) != bytes) {
			result = -EFAULT;
			goto consume;
		}
		copied += bytes;
	}
	message->msg_flags = copied < length ? MSG_TRUNC : 0;
	message->msg_controllen = 0;
	if (message->msg_name) {
		memcpy(message->msg_name, &address,
		       MIN(message->msg_namelen, sizeof(address)));
		message->msg_namelen = sizeof(address);
	}
	result = flags & MSG_TRUNC ? (ssize_t)length : (ssize_t)copied;
consume:
	if (!(flags & MSG_PEEK)) {
		uk_list_del(&packet->link);
		context->receive.count--;
		context->receive.bytes -= packet->charge;
		if (!context->receive.count)
			posix_sock_event_clear(socket, EPOLLIN);
		nl_message_free(context, packet);
	}
out:
	uk_mutex_unlock(&context->receive_lock);
	return result;
}

static ssize_t nl_recvfrom(posix_sock *socket, void *buffer, size_t length,
			   int flags, struct sockaddr *address,
			   socklen_t *address_length)
{
	struct iovec vector = {buffer, length};
	struct msghdr message = {.msg_name = address,
				 .msg_namelen = address ? *address_length : 0,
				 .msg_iov = &vector,
				 .msg_iovlen = 1};
	ssize_t result = nl_recvmsg(socket, &message, flags);

	if (result >= 0 && address)
		*address_length = message.msg_namelen;
	return result;
}

static void *nl_accept4(posix_sock *s __unused, struct sockaddr *addr __unused,
			socklen_t *addr_len __unused, int flags __unused)
{
	return ERR2PTR(-EOPNOTSUPP);
}

static int nl_shutdown(posix_sock *s __unused, int how __unused)
{
	return -ENOTCONN;
}

static int nl_getsockopt(posix_sock *s __unused, int lvl __unused,
			 int opt __unused, void *optval __unused,
			 socklen_t *optlen __unused)
{
	UK_WARN_STUBBED();
	return -ENOPROTOOPT;
}

static int nl_setsockopt(posix_sock *s __unused, int lvl __unused,
			 int opt __unused, const void *val __unused,
			 socklen_t optlen __unused)
{
	return -ENOPROTOOPT;
}

static int nl_listen(posix_sock *s __unused, int backlog __unused)
{
	return -EOPNOTSUPP;
}

static ssize_t nl_sendmsg(posix_sock *socket, const struct msghdr *message,
			  int flags)
{
	return nl_sendv(socket, message->msg_iov, message->msg_iovlen, flags,
			message->msg_name, message->msg_namelen);
}

static int nl_sockpair(struct posix_socket_driver *d __unused,
		       int family __unused, int type __unused,
		       int prot __unused, void *usockvec[2] __unused)
{
	return -EOPNOTSUPP;
}

static int nl_ioctl(posix_sock *socket, int request, void *argument)
{
	struct nl_ctx *context = sock2nlctx(socket);
	struct nl_message *packet;
	int size;

	if (request != FIONREAD)
		return -EINVAL;
	uk_mutex_lock(&context->receive_lock);
	packet = uk_list_first_entry_or_null(&context->receive.messages,
					     struct nl_message, link);
	size = packet ? (int)nlbuf_len(packet->buffer) : 0;
	uk_mutex_unlock(&context->receive_lock);
	return uk_nofault_try_write(size, argument) ? 0 : -EFAULT;
}

static void nl_poll_setup(posix_sock *socket)
{
	struct nl_ctx *context = sock2nlctx(socket);

	context->socket = socket;
	posix_sock_event_assign(socket, EPOLLOUT);
}

static const struct posix_socket_ops netlink_vops = {
	.nofault_io = true,
	.create = nl_create,
	.accept4 = nl_accept4,
	.bind = nl_bind,
	.shutdown = nl_shutdown,
	.getpeername = nl_getpeername,
	.getsockname = nl_sockname,
	.getsockopt = nl_getsockopt,
	.setsockopt = nl_setsockopt,
	.connect = nl_connect,
	.listen = nl_listen,
	.recvfrom = nl_recvfrom,
	.recvmsg = nl_recvmsg,
	.sendmsg = nl_sendmsg,
	.sendto = nl_sendto,
	.socketpair = nl_sockpair,
	.close = nl_close,
	.ioctl = nl_ioctl,
	.poll_setup = nl_poll_setup,
};

POSIX_SOCKET_FAMILY_REGISTER(AF_NETLINK, &netlink_vops);

#if CONFIG_LIBPOSIX_NETLINK_TEST
#include "tests/test_transport.c"
#endif
