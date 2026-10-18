/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_NETLINK_DRIVER_H__
#define __UK_NETLINK_DRIVER_H__

#include <uk/essentials.h>
#include <uk/socket_driver.h>
#include <linux/netlink.h>
#include <uk/atomic.h>
#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/streambuf.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Netlink context and driver helpers */

struct nl_ctx;

struct nl_reply_batch {
	struct nl_ctx *context;
	struct uk_list_head messages;
	size_t bytes, count;
};

struct nl_ctx {
	const struct posix_netlink_protocol *drv;
	struct uk_alloc *allocator;
	struct nl_reply_batch receive;
	struct uk_mutex receive_lock;
	struct uk_list_head port_link;
	posix_sock *socket;
	__u32 flags;
	__u32 nl_groups;
	__u32 nl_pid;
};

static inline __u32 nl_ctx_pid(const struct nl_ctx *ctx)
{
	return uk_load_n(&ctx->nl_pid);
}

static inline struct uk_streambuf *nlbuf_alloc(struct nl_ctx *ctx, size_t len)
{
	return uk_streambuf_alloc2(ctx->allocator, len, sizeof(long),
				   UK_STREAMBUF_C_WIPEZERO);
}

#define nlbuf_reserve(nlbuf, len) uk_streambuf_reserve((nlbuf), (len))
#define nlbuf_free(nlbuf) uk_streambuf_free((nlbuf))
#define nlbuf_len(nlbuf) uk_streambuf_seek((nlbuf))
#define nlbuf_data(nlbuf) uk_streambuf_buf((nlbuf))

static inline void *nlbuf_align(struct uk_streambuf *nlbuf, size_t align)
{
	const size_t seek = uk_streambuf_seek(nlbuf);

	return uk_streambuf_reserve(nlbuf, ALIGN_UP(seek, align) - seek);
}

/* Takes ownership of nlbuf, including on error. */
int nlbuf_send(struct nl_reply_batch *batch, struct uk_streambuf *nlbuf);

/* Netlink driver ops & driver registration */

/**
 * REQUIRED. Handle netlink requests on a socket.
 *
 * Drivers append owned replies to the batch. The socket publishes the batch
 * after successful request handling, or releases it on error.
 *
 * @param ctx Netlink context of the socket
 * @param batch Owned response batch
 * @param nlh Owned netlink request message
 *
 * @return 0 on success, negative errno otherwise
 */
typedef int (*posix_netlink_protocol_handle_func_t)(struct nl_ctx *ctx,
				struct nl_reply_batch *batch,
				const struct nlmsghdr *nlh);

/**
 * OPTIONAL. Callback on creating a netlink socket.
 *
 * @param sock Reference to the socket
 * @param addr The address of the peer socket
 * @param addr_len Specifies the size, in bytes, of the address structure
 *    pointed to by addr
 * @param flags Additional flags to be set for the accepted connection. If
 *    flags is 0, accept4 is the same as POSIX accept.
 *
 * @return 0 on success, negative errno otherwise
 */
typedef int (*posix_netlink_protocol_create_func_t)(struct nl_ctx *ctx);

/**
 * OPTIONAL. Callback on closing a netlink socket.
 *
 * @param ctx Netlink context of the socket
 */
typedef void (*posix_netlink_protocol_close_func_t)(struct nl_ctx *ctx);

struct posix_netlink_protocol_ops {
	posix_netlink_protocol_handle_func_t handle;
	posix_netlink_protocol_create_func_t create;
	posix_netlink_protocol_close_func_t close;
};

struct posix_netlink_protocol {
	/** The interfaces for this socket */
	const struct posix_netlink_protocol_ops *ops;
	/** Name of the driver library */
	const char *libname;
	/** The netlink protocol ID */
	const int protocol;
} __align(8);

#define _POSIX_NETLINK_PROTOCOL_DRVRNAME(lib, prot)                            \
	posix_netlink_protocol_##lib##_##prot

#define _POSIX_NETLINK_PROTOCOL_SECNAME(lib, prot)                             \
	STRINGIFY(_POSIX_NETLINK_PROTOCOL_DRVRNAME(lib, prot))

/* Register a netlink driver part of `lib` with `vops` for AF `prot`. */
#define _POSIX_NETLINK_PROTOCOL_REGISTER(lib, prot, vops)		\
	__used __align(8)						\
		__section("." _POSIX_NETLINK_PROTOCOL_SECNAME(lib, prot))\
	static const struct posix_netlink_protocol			\
	_POSIX_NETLINK_PROTOCOL_DRVRNAME(lib, prot) = {			\
		.protocol = (prot),					\
		.libname = STRINGIFY(lib),				\
		.ops = (vops)						\
	}

#define POSIX_NETLINK_PROTOCOL_REGISTER(prot, vops)                            \
	_POSIX_NETLINK_PROTOCOL_REGISTER(__LIBNAME__, prot, vops)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __UK_NETLINK_DRIVER_H__ */
