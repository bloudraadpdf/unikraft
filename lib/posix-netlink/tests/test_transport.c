/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/test.h>

#define NETLINK_TEST_PROTOCOL 1024

static int transport_echo(struct nl_ctx *context, struct nl_reply_batch *batch,
			  const struct nlmsghdr *request)
{
	struct uk_streambuf *reply;
	void *data;

	if (request->nlmsg_type == NLMSG_ERROR)
		return -EINVAL;
	reply = nlbuf_alloc(context, request->nlmsg_len);
	if (!reply)
		return -ENOMEM;
	data = nlbuf_reserve(reply, request->nlmsg_len);
	UK_ASSERT(data);
	memcpy(data, request, request->nlmsg_len);
	return nlbuf_send(batch, reply);
}

static const struct posix_netlink_protocol_ops transport_ops = {
	.handle = transport_echo,
};

POSIX_NETLINK_PROTOCOL_REGISTER(NETLINK_TEST_PROTOCOL, &transport_ops);

static struct uk_file *transport_socket(void)
{
	struct uk_file *socket =
		uk_socket_create(AF_NETLINK, SOCK_RAW, NETLINK_TEST_PROTOCOL);

	return PTRISERR(socket) ? NULL : socket;
}

static ssize_t transport_send(posix_sock *socket, const void *data,
			      size_t length)
{
	const struct sockaddr_nl address = {.nl_family = AF_NETLINK};

	return nl_sendto(socket, data, length, 0,
			 (const struct sockaddr *)&address, sizeof(address));
}

UK_TESTCASE(netlink_transport, split_send_peek_truncation_and_readiness)
{
	struct uk_file *socket = transport_socket();
	struct {
		struct nlmsghdr header;
		unsigned long payload;
	} request = {.header = {.nlmsg_len = sizeof(request),
				.nlmsg_type = NLMSG_NOOP},
		     .payload = 0x12345678};
	struct sockaddr_nl address = {.nl_family = AF_NETLINK};
	struct iovec vectors[] = {{&request.header, sizeof(request.header)},
				  {&request.payload, sizeof(request.payload)}};
	struct msghdr message = {.msg_name = &address,
				 .msg_namelen = sizeof(address),
				 .msg_iov = vectors,
				 .msg_iovlen = ARRAY_SIZE(vectors)};
	unsigned char result[sizeof(request)];

	UK_TEST_EXPECT_NOT_NULL(socket);
	if (!socket)
		return;
	UK_TEST_EXPECT_SNUM_EQ(nl_sendmsg(socket, &message, 0),
			       sizeof(request));
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN),
			       EPOLLIN);
	vectors[0] = (struct iovec){result, sizeof(struct nlmsghdr)};
	message = (struct msghdr){.msg_iov = vectors, .msg_iovlen = 1};
	UK_TEST_EXPECT_SNUM_EQ(nl_recvmsg(socket, &message,
					  MSG_PEEK | MSG_TRUNC),
			       sizeof(request));
	UK_TEST_EXPECT_SNUM_EQ(message.msg_flags, MSG_TRUNC);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN),
			       EPOLLIN);
	vectors[0].iov_len = sizeof(result);
	UK_TEST_EXPECT_SNUM_EQ(nl_recvmsg(socket, &message, 0),
			       sizeof(request));
	UK_TEST_EXPECT_SNUM_EQ(memcmp(result, &request, sizeof(request)), 0);
	UK_TEST_EXPECT_SNUM_EQ(message.msg_flags, 0);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN), 0);
	UK_TEST_EXPECT_SNUM_EQ(nl_recvmsg(socket, &message, MSG_DONTWAIT),
			       -EAGAIN);
	uk_file_release(socket);
}

UK_TESTCASE(netlink_transport, request_fault_and_batch_failure_publish_nothing)
{
	struct uk_file *socket = transport_socket();
	struct nlmsghdr requests[] = {
		{.nlmsg_len = sizeof(struct nlmsghdr),
		 .nlmsg_type = NLMSG_NOOP},
		{.nlmsg_len = sizeof(struct nlmsghdr),
		 .nlmsg_type = NLMSG_ERROR},
	};

	UK_TEST_EXPECT_NOT_NULL(socket);
	if (!socket)
		return;
	UK_TEST_EXPECT_SNUM_EQ(transport_send(socket, (void *)1,
					      sizeof(requests)),
			       -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN), 0);
	UK_TEST_EXPECT_SNUM_EQ(transport_send(socket, requests,
					      sizeof(requests)),
			       -EINVAL);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN), 0);
	uk_file_release(socket);
}

UK_TESTCASE(netlink_transport,
	    faulting_peek_retains_and_faulting_receive_consumes)
{
	struct uk_file *socket = transport_socket();
	struct nlmsghdr request = {.nlmsg_len = sizeof(request),
				   .nlmsg_type = NLMSG_NOOP};
	struct iovec vector = {(void *)1, sizeof(request)};
	struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1};

	UK_TEST_EXPECT_NOT_NULL(socket);
	if (!socket)
		return;
	UK_TEST_EXPECT_SNUM_EQ(transport_send(socket, &request,
					      sizeof(request)),
			       sizeof(request));
	UK_TEST_EXPECT_SNUM_EQ(nl_recvmsg(socket, &message, MSG_PEEK), -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN),
			       EPOLLIN);
	UK_TEST_EXPECT_SNUM_EQ(nl_recvmsg(socket, &message, 0), -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(uk_file_poll_immediate(socket, EPOLLIN), 0);
	uk_file_release(socket);
}

UK_TESTCASE(netlink_transport, full_reply_queue_returns_without_partial_commit)
{
	struct uk_file *socket = transport_socket();
	struct nlmsghdr request = {.nlmsg_len = sizeof(request),
				   .nlmsg_type = NLMSG_NOOP};
	struct nl_ctx *context;

	UK_TEST_EXPECT_NOT_NULL(socket);
	if (!socket)
		return;
	context = sock2nlctx(socket);
	for (unsigned int i = 0; i < NL_RECV_MAX_MESSAGES; i++)
		UK_TEST_EXPECT_SNUM_EQ(transport_send(socket, &request,
						      sizeof(request)),
				       sizeof(request));
	UK_TEST_EXPECT_SNUM_EQ(context->receive.count, NL_RECV_MAX_MESSAGES);
	UK_TEST_EXPECT_SNUM_EQ(transport_send(socket, &request,
					      sizeof(request)),
			       -ENOBUFS);
	UK_TEST_EXPECT_SNUM_EQ(context->receive.count, NL_RECV_MAX_MESSAGES);
	uk_file_release(socket);
}

UK_TESTCASE(netlink_transport,
	    auto_bound_ports_are_unique_and_explicit_conflicts_fail)
{
	struct uk_file *first = transport_socket();
	struct uk_file *second = transport_socket();
	struct sockaddr_nl address = {.nl_family = AF_NETLINK};
	struct nl_ctx *a, *b;

	UK_TEST_EXPECT_NOT_NULL(first);
	UK_TEST_EXPECT_NOT_NULL(second);
	if (!first || !second)
		goto out;
	a = sock2nlctx(first);
	b = sock2nlctx(second);
	UK_TEST_EXPECT_SNUM_EQ(nl_ctx_pid(a), 0);
	UK_TEST_EXPECT_SNUM_EQ(nl_bind(first, (struct sockaddr *)&address,
				       sizeof(address)),
			       0);
	address.nl_pid = nl_ctx_pid(a);
	UK_TEST_EXPECT_SNUM_EQ(nl_bind(second, (struct sockaddr *)&address,
				       sizeof(address)),
			       -EADDRINUSE);
	address.nl_pid = 0;
	UK_TEST_EXPECT_SNUM_EQ(nl_bind(second, (struct sockaddr *)&address,
				       sizeof(address)),
			       0);
	UK_TEST_EXPECT_NOT_ZERO(nl_ctx_pid(a));
	UK_TEST_EXPECT_NOT_ZERO(nl_ctx_pid(b));
	UK_TEST_EXPECT_NOT_ZERO(nl_ctx_pid(a) != nl_ctx_pid(b));
out:
	if (first)
		uk_file_release(first);
	if (second)
		uk_file_release(second);
}

UK_TESTSUITE_AT_APPINIT(netlink_transport, NULL);
