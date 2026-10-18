/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include "lwip/opt.h"

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <netinet/in.h>

#include <uk/bitops/bitscan.h>
#include <uk/netlink/driver.h>
#include <uk/print.h>
#include <uk/streambuf.h>

#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <linux/if_arp.h> /* ARPHRD_ETHER */
#include <lwip/tcp.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>

static void uk_pr_debug_nlh(const struct nlmsghdr *nlh)
{
	uk_pr_debug("nlh->len: %"PRIu32"\n", nlh->nlmsg_len);
	uk_pr_debug("nlh->type: %"PRIu16"\n", nlh->nlmsg_type);
	uk_pr_debug("nlh->flags: 0x%"PRIx16"\n", nlh->nlmsg_flags);
	uk_pr_debug("nlh->seq: %"PRIu32"\n", nlh->nlmsg_seq);
	uk_pr_debug("nlh->pid: %"PRIu32"\n", nlh->nlmsg_pid);
}

/**
 * Append an rtattr to a netlink reply.
 *
 * @return pointer to payload to be filled, NULL if there is no space left
 */
static void *nlbuf_rtattr(struct uk_streambuf *rep, unsigned short rta_type,
			  unsigned short data_len)
{
	struct rtattr *attr;

	/* place rtattr * structure */
	attr = nlbuf_reserve(rep, RTA_SPACE(data_len));
	if (unlikely(!attr))
		return NULL;

	attr->rta_type = rta_type;
	attr->rta_len = RTA_LENGTH(data_len);
	return RTA_DATA(attr);
}

#if MIB2_STATS
static inline bool netif_is_loopback(const struct netif *netif)
{
	return netif->type == snmp_ifType_softwareLoopback;
}
#else /* !MIB2_STATS */
static inline bool netif_is_loopback(const struct netif *netif)
{
	bool ret = false;

#if LWIP_HAVE_LOOPIF
	/* Unfortunately, for loopback interfaces we have to guess
	 * and check for a parameter setup that is a result of
	 * `netif_loopif_init()`
	 */
	ret = netif->name[0] == 'l' && netif->name[1] == 'o';
#if LWIP_CHECKSUM_CTRL_PER_NETIF
	ret = ret && netif->chksum_flags == NETIF_CHECKSUM_DISABLE_ALL;
#endif /* LWIP_CHECKSUM_CTRL_PER_NETIF */
#endif /* LWIP_HAVE_LOOPIF */
	return ret;
}
#endif /* !MIB2_STATS */

#define NLMSG_ERR_LEN NLMSG_LENGTH(sizeof(struct nlmsgerr))
#define NLMSG_DONE_LEN NLMSG_LENGTH(sizeof(int))

/* Only prepares msg; does not send msg, does not assume streambuf is empty */
static void prep_reply_err(struct nl_ctx *ctx, struct uk_streambuf *sb, int err,
			   uint16_t flags, const struct nlmsghdr *req)
{
	struct nlmsghdr *nlh;
	struct nlmsgerr *nlerr;

	uk_pr_debug("Reply with error msg: %d\n", err);

	nlh = nlbuf_reserve(sb, NLMSG_HDRLEN);
	UK_ASSERT(nlh);
	nlh->nlmsg_type = NLMSG_ERROR;
	nlh->nlmsg_flags = flags;
	nlh->nlmsg_pid = nl_ctx_pid(ctx);
	nlh->nlmsg_seq = req->nlmsg_seq;
	nlh->nlmsg_len = NLMSG_ERR_LEN;
	uk_pr_debug_nlh(nlh);

	nlerr = nlbuf_reserve(sb, NLMSG_ALIGN(sizeof(*nlerr)));
	UK_ASSERT(nlerr);
	nlerr->error = err;
	nlerr->msg = *req;
}

/* Only prepares msg; does not send msg, does not assume streambuf is empty */
static void prep_reply_done(struct nl_ctx *ctx, struct uk_streambuf *sb,
			    const struct nlmsghdr *req)
{
	struct nlmsghdr *nlh;
	int *error;

	uk_pr_debug("Reply with DONE message\n");

	nlh = nlbuf_reserve(sb, NLMSG_HDRLEN);
	UK_ASSERT(nlh);

	nlh->nlmsg_type = NLMSG_DONE;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_pid = nl_ctx_pid(ctx);
	nlh->nlmsg_seq = req->nlmsg_seq;
	nlh->nlmsg_len = NLMSG_DONE_LEN;
	error = nlbuf_reserve(sb, sizeof(*error));
	UK_ASSERT(error);
	*error = 0;
	uk_pr_debug_nlh(nlh);
}

static int reply_getlink_if(struct nl_ctx *ctx, struct nl_reply_batch *batch,
			    struct netif *netif, const struct nlmsghdr *req)
{
	struct uk_streambuf *rep;
	struct nlmsghdr *nlh;
	struct ifinfomsg *ifi;
	char *ifname;
	char name[NETIF_NAMESIZE];
	uint32_t *ifmtu;
	char *ifaddr;

	if (!netif_index_to_name(netif_get_index(netif), name))
		return -ENODEV;
	rep = nlbuf_alloc(ctx, 512);
	if (unlikely(!rep))
		return -ENOMEM;

	uk_pr_debug("Generate getlink for %c%c%d\n", netif->name[0],
		    netif->name[1], netif->num);
	nlh = nlbuf_reserve(rep, NLMSG_HDRLEN);
	UK_ASSERT(nlh);
	nlh->nlmsg_type = RTM_NEWLINK;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_pid = nl_ctx_pid(ctx);
	nlh->nlmsg_seq = req->nlmsg_seq;

	/* IFINFO */
	ifi = nlbuf_reserve(rep, NLMSG_ALIGN(sizeof(*ifi)));
	UK_ASSERT(ifi);
#if MIB2_STATS
	switch (netif->type) {
	case snmp_ifType_softwareLoopback:
		ifi->ifi_type = ARPHRD_LOOPBACK;
		break;
	case snmp_ifType_ppp:
		ifi->ifi_type = ARPHRD_PPP;
		break;
	case snmp_ifType_slip:
		ifi->ifi_type = ARPHRD_SLIP;
		break;
	default:
		ifi->ifi_type = ARPHRD_ETHER;
		break;
	}
#else  /* !MIB2_STATS */
	if (netif_is_loopback(netif)) {
		ifi->ifi_type = ARPHRD_LOOPBACK;
	} else if (netif->flags & NETIF_FLAG_ETHERNET) {
		ifi->ifi_type = ARPHRD_ETHER;
	} else {
		ifi->ifi_type = ARPHRD_VOID;
	}
#endif /* !MIB2_STATS */
	ifi->ifi_index = netif_get_index(netif);
	ifi->ifi_family = AF_UNSPEC;
	ifi->ifi_flags = 0;
	if (netif_is_link_up(netif))
		ifi->ifi_flags |= IFF_RUNNING | IFF_LOWER_UP;
	if (netif_is_loopback(netif))
		ifi->ifi_flags |= IFF_LOOPBACK;
	if (netif->flags & (NETIF_FLAG_IGMP | NETIF_FLAG_MLD6))
		ifi->ifi_flags |= IFF_MULTICAST;
	ifi->ifi_change = 0xffffffff;
	if (netif->flags & NETIF_FLAG_UP)
		ifi->ifi_flags |= IFF_UP;
	if (netif->flags & NETIF_FLAG_BROADCAST)
		ifi->ifi_flags |= IFF_BROADCAST;

	/* IFLA_IFNAME (C-string) */
	uk_pr_debug("Append IFLA_IFNAME\n");
	ifname = nlbuf_rtattr(rep, IFLA_IFNAME, strlen(name) + 1);
	UK_ASSERT(ifname);
	memcpy(ifname, name, strlen(name) + 1);

	/* IFLA_MTU (u32) */
	uk_pr_debug("Append IFLA_MTU\n");
	ifmtu = nlbuf_rtattr(rep, IFLA_MTU, sizeof(*ifmtu));
	UK_ASSERT(ifmtu);
	*ifmtu = netif->mtu;

	/* IFLA_ADDRESS (hwaddr) */
	if (netif->hwaddr_len && (ifi->ifi_type != ARPHRD_LOOPBACK)) {
		uk_pr_debug("Append IFLA_ADDRESS\n");
		ifaddr = nlbuf_rtattr(rep, IFLA_ADDRESS, netif->hwaddr_len);
		UK_ASSERT(ifaddr);
		for (unsigned int i = 0; i < netif->hwaddr_len; i++)
			ifaddr[i] = netif->hwaddr[i];
	}

	nlh->nlmsg_len = nlbuf_len(rep);

	uk_pr_debug("Put getlink message for %c%c%d\n", netif->name[0],
		    netif->name[1], netif->num);
	uk_pr_debug_nlh(nlh);

	return nlbuf_send(batch, rep);
}

static int reply_dump(struct nl_ctx *ctx, struct nl_reply_batch *batch,
		      const struct nlmsghdr *request,
		      int (*reply)(struct nl_ctx *, struct nl_reply_batch *,
				   struct netif *, const struct nlmsghdr *))
{
	struct uk_streambuf *end;
	struct netif *interface;
	int error = 0;

	end = nlbuf_alloc(ctx, NLMSG_DONE_LEN + NLMSG_ERR_LEN);
	if (!end)
		return -ENOMEM;
	if ((request->nlmsg_flags & (NLM_F_REQUEST | NLM_F_DUMP)) !=
	    (NLM_F_REQUEST | NLM_F_DUMP)) {
		error = -EINVAL;
	} else if (reply) {
		for (interface = netif_list; interface;
		     interface = interface->next) {
			error = reply(ctx, batch, interface, request);
			if (error)
				break;
		}
	}
	if (error)
		prep_reply_err(ctx, end, error, NLM_F_MULTI, request);
	prep_reply_done(ctx, end, request);
	return nlbuf_send(batch, end);
}

#if LWIP_IPV4
/* The following code assumes that long is at least a 32 bit number */
UK_CTASSERT(sizeof(long) >= sizeof(uint32_t));

static inline uint8_t netmask_2_suffixlen(const ip4_addr_t *netmask)
{
	long hmask;

	UK_ASSERT(netmask);

	/* Corner cases */
	if ((unsigned long)netmask->addr == 0x0UL)
		return 32;
	if ((unsigned long)netmask->addr == 0xFFFFFFFFUL)
		return 0;

	/* Suffix length is equal to the least significant bit that is `1` */
	hmask = (long)PP_NTOHL(netmask->addr);
	return (uint8_t)uk_lssbl(hmask);
}

#define netmask_2_prefixlen(netmask) (32 - netmask_2_suffixlen(netmask))

static inline void ip4_addr_2_in_addr(const ip4_addr_t *in, in_addr_t *out)
{
	UK_ASSERT(in);
	UK_ASSERT(out);

	/* Both data structures are in network byte order */
	/* Both data structures consists of a __u32 value */
	*((uint32_t *)out) = (uint32_t)in->addr;
}

static int reply_getaddr_if(struct nl_ctx *ctx, struct nl_reply_batch *batch,
			    struct netif *netif, const struct nlmsghdr *req)
{
	struct uk_streambuf *rep;
	struct nlmsghdr *nlh;
	struct ifaddrmsg *ifa;
	in_addr_t *inaddr;
	in_addr_t *inbc;
	char *ifname;
	char name[NETIF_NAMESIZE];

	if (ip4_addr_isany_val(*netif_ip4_addr(netif)))
		return 0;
	if (!netif_index_to_name(netif_get_index(netif), name))
		return -ENODEV;

	rep = nlbuf_alloc(ctx, 256);
	if (unlikely(!rep))
		return -ENOMEM;

	uk_pr_debug("Generate getaddr for %c%c%d\n", netif->name[0],
		    netif->name[1], netif->num);
	nlh = nlbuf_reserve(rep, NLMSG_HDRLEN);
	UK_ASSERT(nlh);
	nlh->nlmsg_type = RTM_NEWADDR;
	nlh->nlmsg_flags = NLM_F_MULTI;
	nlh->nlmsg_pid = nl_ctx_pid(ctx);
	nlh->nlmsg_seq = req->nlmsg_seq;

	ifa = nlbuf_reserve(rep, NLMSG_ALIGN(sizeof(*ifa)));
	UK_ASSERT(ifa);
	ifa->ifa_index = netif_get_index(netif);
	ifa->ifa_family = AF_INET;
	if (netif_is_loopback(netif)) {
		ifa->ifa_flags = IFA_F_PERMANENT;
		ifa->ifa_scope = RT_SCOPE_HOST;
	} else {
		ifa->ifa_flags = 0x0;
		ifa->ifa_scope = RT_SCOPE_UNIVERSE;
	}
	ifa->ifa_prefixlen = netmask_2_prefixlen(netif_ip4_netmask(netif));
	inaddr = nlbuf_rtattr(rep, IFA_ADDRESS, sizeof(*inaddr));
	UK_ASSERT(inaddr);
	ip4_addr_2_in_addr(netif_ip4_addr(netif), inaddr);
	inaddr = nlbuf_rtattr(rep, IFA_LOCAL, sizeof(*inaddr));
	UK_ASSERT(inaddr);
	ip4_addr_2_in_addr(netif_ip4_addr(netif), inaddr);
	if (netif->flags & NETIF_FLAG_BROADCAST) {
		inbc = nlbuf_rtattr(rep, IFA_BROADCAST, sizeof(*inbc));
		UK_ASSERT(inbc);
		*inbc = ip4_addr_get_u32(netif_ip4_addr(netif)) |
			~ip4_addr_get_u32(netif_ip4_netmask(netif));
	}

	/* IFA_LABEL (C-string) */
	uk_pr_debug("Append IFA_LABEL\n");
	ifname = nlbuf_rtattr(rep, IFA_LABEL, strlen(name) + 1);
	UK_ASSERT(ifname);
	memcpy(ifname, name, strlen(name) + 1);

	nlh->nlmsg_len = nlbuf_len(rep);

	uk_pr_debug("Put getaddr message for %c%c%d\n", netif->name[0],
		    netif->name[1], netif->num);
	uk_pr_debug_nlh(nlh);

	return nlbuf_send(batch, rep);
}

#endif /* LWIP_IPV4 */

static int reply_enotsup(struct nl_ctx *ctx, struct nl_reply_batch *batch,
			 const struct nlmsghdr *req)
{
	struct uk_streambuf *rep;

	rep = nlbuf_alloc(ctx, NLMSG_ERR_LEN);
	if (unlikely(!rep))
		return -ENOMEM;

	prep_reply_err(ctx, rep, -ENOTSUP, 0, req);
	return nlbuf_send(batch, rep);
}

struct route_query {
	struct tcpip_api_call_data call;
	struct nl_ctx *context;
	struct nl_reply_batch *batch;
	const struct nlmsghdr *request;
	int result;
};

static err_t route_query_core(struct tcpip_api_call_data *call)
{
	struct route_query *query =
		__containerof(call, struct route_query, call);
	const struct nlmsghdr *request = query->request;
	struct nl_ctx *context = query->context;
	struct nl_reply_batch *batch = query->batch;
	unsigned int family;

	if (request->nlmsg_len < NLMSG_LENGTH(sizeof(struct rtgenmsg))) {
		query->result = reply_enotsup(context, batch, request);
		return ERR_OK;
	}
	family = ((const struct rtgenmsg *)NLMSG_DATA(request))->rtgen_family;
	switch (request->nlmsg_type) {
	case RTM_GETLINK:
		query->result =
			reply_dump(context, batch, request, reply_getlink_if);
		break;
	case RTM_GETADDR:
		if (family == AF_UNSPEC || family == AF_INET) {
#if LWIP_IPV4
			query->result = reply_dump(context, batch, request,
						   reply_getaddr_if);
#else
			query->result =
				reply_dump(context, batch, request, NULL);
#endif
		} else {
			query->result = reply_enotsup(context, batch, request);
		}
		break;
	default:
		query->result = reply_enotsup(context, batch, request);
		break;
	}
	return ERR_OK;
}

static int lwip_nl_route_handle(struct nl_ctx *context,
				struct nl_reply_batch *batch,
				const struct nlmsghdr *request)
{
	struct route_query query = {
		.context = context,
		.batch = batch,
		.request = request,
	};
	err_t error = tcpip_api_call(route_query_core, &query.call);

	return error == ERR_OK	  ? query.result
	       : error == ERR_MEM ? -ENOMEM
				  : -EIO;
}

static const struct posix_netlink_protocol_ops lwip_nl_route_ops = {
	.handle = lwip_nl_route_handle};

POSIX_NETLINK_PROTOCOL_REGISTER(NETLINK_ROUTE, &lwip_nl_route_ops);
