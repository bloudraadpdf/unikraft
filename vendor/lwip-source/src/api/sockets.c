/**
 * @file
 * Sockets BSD-Like API module
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 * Improved by Marc Boucher <marc@mbsi.ca> and David Haas <dhaas@alum.rpi.edu>
 *
 */

#define _BSD_SOURCE

#include <sys/time.h>
#include <uk/nofault.h>
#include <uk/alloc.h>
#include <uk/atomic.h>

#include "lwip/opt.h"

#if LWIP_SOCKET /* don't build if not configured for use in lwipopts.h */

#include "lwip/sockets.h"
#include "lwip/priv/api_msg.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/api.h"
#include "lwip/igmp.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/raw.h"
#include "lwip/udp.h"
#include "lwip/memp.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/mld6.h"
#if LWIP_CHECKSUM_ON_COPY
#include "lwip/inet_chksum.h"
#endif

#if LWIP_COMPAT_SOCKETS == 2 && LWIP_POSIX_SOCKETS_IO_NAMES
#include <stdarg.h>
#endif

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>

#ifdef LWIP_HOOK_FILENAME
#include LWIP_HOOK_FILENAME
#endif

/* If the netconn API is not required publicly, then we include the necessary
   files here to get the implementation */
#if !LWIP_NETCONN
#undef LWIP_NETCONN
#define LWIP_NETCONN 1
#include "api_msg.c"
#include "api_lib.c"
#include "netbuf.c"
#undef LWIP_NETCONN
#define LWIP_NETCONN 0
#endif

#define API_SELECT_CB_VAR_REF(name)               API_VAR_REF(name)
#define API_SELECT_CB_VAR_DECLARE(name)           API_VAR_DECLARE(struct lwip_select_cb, name)
#define API_SELECT_CB_VAR_ALLOC(name, retblock)   API_VAR_ALLOC_EXT(struct lwip_select_cb, MEMP_SELECT_CB, name, retblock)
#define API_SELECT_CB_VAR_FREE(name)              API_VAR_FREE(MEMP_SELECT_CB, name)

#if LWIP_IPV4
#define SIN_ZERO_LEN 8
#define IP4ADDR_PORT_TO_SOCKADDR(sin, ipaddr, port) do { \
      (sin)->sin_family = AF_INET; \
      (sin)->sin_port = lwip_htons((port)); \
      inet_addr_from_ip4addr(&(sin)->sin_addr, ipaddr); \
      memset((sin)->sin_zero, 0, SIN_ZERO_LEN); }while(0)
#define SOCKADDR4_TO_IP4ADDR_PORT(sin, ipaddr, port) do { \
    inet_addr_to_ip4addr(ip_2_ip4(ipaddr), &((sin)->sin_addr)); \
    (port) = lwip_ntohs((sin)->sin_port); }while(0)
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
#define IP6ADDR_PORT_TO_SOCKADDR(sin6, ipaddr, port) do { \
      (sin6)->sin6_family = AF_INET6; \
      (sin6)->sin6_port = lwip_htons((port)); \
      (sin6)->sin6_flowinfo = 0; \
      inet6_addr_from_ip6addr(&(sin6)->sin6_addr, ipaddr); \
      (sin6)->sin6_scope_id = ip6_addr_zone(ipaddr); }while(0)
#define SOCKADDR6_TO_IP6ADDR_PORT(sin6, ipaddr, port) do { \
    inet6_addr_to_ip6addr(ip_2_ip6(ipaddr), &((sin6)->sin6_addr)); \
    if (ip6_addr_has_scope(ip_2_ip6(ipaddr), IP6_UNKNOWN)) { \
      ip6_addr_set_zone(ip_2_ip6(ipaddr), (u8_t)((sin6)->sin6_scope_id)); \
    } \
    (port) = lwip_ntohs((sin6)->sin6_port); }while(0)
#endif /* LWIP_IPV6 */

#if LWIP_IPV4 && LWIP_IPV6

#define IPADDR_PORT_TO_SOCKADDR(sockaddr, ipaddr, port) do { \
    if (IP_IS_ANY_TYPE_VAL(*ipaddr) || IP_IS_V6_VAL(*ipaddr)) { \
      IP6ADDR_PORT_TO_SOCKADDR((struct sockaddr_in6*)(void*)(sockaddr), ip_2_ip6(ipaddr), port); \
    } else { \
      IP4ADDR_PORT_TO_SOCKADDR((struct sockaddr_in*)(void*)(sockaddr), ip_2_ip4(ipaddr), port); \
    } } while(0)
#define DOMAIN_TO_NETCONN_TYPE(domain, type) (((domain) == AF_INET) ? \
  (type) : (enum netconn_type)((type) | NETCONN_TYPE_IPV6))
#elif LWIP_IPV6 /* LWIP_IPV4 && LWIP_IPV6 */
#define IPADDR_PORT_TO_SOCKADDR(sockaddr, ipaddr, port) \
        IP6ADDR_PORT_TO_SOCKADDR((struct sockaddr_in6*)(void*)(sockaddr), ip_2_ip6(ipaddr), port)
#define DOMAIN_TO_NETCONN_TYPE(domain, netconn_type) (netconn_type)
#else /*-> LWIP_IPV4: LWIP_IPV4 && LWIP_IPV6 */
#define IPADDR_PORT_TO_SOCKADDR(sockaddr, ipaddr, port) \
        IP4ADDR_PORT_TO_SOCKADDR((struct sockaddr_in*)(void*)(sockaddr), ip_2_ip4(ipaddr), port)
#define DOMAIN_TO_NETCONN_TYPE(domain, netconn_type) (netconn_type)
#endif /* LWIP_IPV6 */

#define IS_SOCK_ADDR_ALIGNED(name)      ((((mem_ptr_t)(name)) % 4) == 0)


union lwip_sockopt_value {
  int integer;
  struct timeval timeout;
  struct linger linger;
  struct ifreq device;
#if LWIP_IPV4
  struct ip_mreq membership;
#endif
#if LWIP_IPV6
  struct ipv6_mreq membership6;
#endif
};

#define LWIP_SOCKOPT_CHECK_OPTLEN(sock, optlen, opttype) do { \
  if (option_input && (optlen) < sizeof(opttype)) { \
    done_socket(sock); return EINVAL; \
  } \
  LWIP_ASSERT("option value fits", sizeof(opttype) <= sizeof(option_value)); \
  option_length = sizeof(opttype); \
  if (option_input && \
      uk_nofault_memcpy((char *)&option_value, (const char *)user_value, \
                        option_length, 0) != option_length) { \
    done_socket(sock); return EFAULT; \
  } \
} while (0)
#define LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, opttype) do { \
  LWIP_SOCKOPT_CHECK_OPTLEN(sock, optlen, opttype); \
  if ((sock)->conn == NULL) { done_socket(sock); return EINVAL; } }while(0)
#define LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, opttype) do { \
  LWIP_SOCKOPT_CHECK_OPTLEN(sock, optlen, opttype); \
  if (((sock)->conn == NULL) || ((sock)->conn->pcb.tcp == NULL)) { done_socket(sock); return EINVAL; } }while(0)
#define LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, opttype, netconntype) do { \
  LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, opttype); \
  if (NETCONNTYPE_GROUP(netconn_type((sock)->conn)) != netconntype) { done_socket(sock); return ENOPROTOOPT; } }while(0)


#define LWIP_SETGETSOCKOPT_DATA_VAR_REF(name)     API_VAR_REF(name)
#define LWIP_SETGETSOCKOPT_DATA_VAR_DECLARE(name) API_VAR_DECLARE(struct lwip_setgetsockopt_data, name)
#define LWIP_SETGETSOCKOPT_DATA_VAR_FREE(name)    API_VAR_FREE(MEMP_SOCKET_SETGETSOCKOPT_DATA, name)
#if LWIP_MPU_COMPATIBLE
#define LWIP_SETGETSOCKOPT_DATA_VAR_ALLOC(name, sock) do { \
  name = (struct lwip_setgetsockopt_data *)memp_malloc(MEMP_SOCKET_SETGETSOCKOPT_DATA); \
  if (name == NULL) { \
    sock_set_errno(sock, ENOMEM); \
    done_socket(sock); \
    return -1; \
  } }while(0)
#else /* LWIP_MPU_COMPATIBLE */
#define LWIP_SETGETSOCKOPT_DATA_VAR_ALLOC(name, sock)
#endif /* LWIP_MPU_COMPATIBLE */

#if LWIP_SO_SNDRCVTIMEO_NONSTANDARD
#define LWIP_SO_SNDRCVTIMEO_OPTTYPE int
#define LWIP_SO_SNDRCVTIMEO_SET(optval, val) (*(int *)(optval) = (val))
#define LWIP_SO_SNDRCVTIMEO_GET_MS(optval)   ((long)*(const int*)(optval))
#else
#define LWIP_SO_SNDRCVTIMEO_OPTTYPE struct timeval
#define LWIP_SO_SNDRCVTIMEO_SET(optval, val)  do { \
  u32_t loc = (val); \
  ((struct timeval *)(optval))->tv_sec = (long)((loc) / 1000U); \
  ((struct timeval *)(optval))->tv_usec = (long)(((loc) % 1000U) * 1000U); }while(0)
#define LWIP_SO_SNDRCVTIMEO_GET_MS(optval) ((((const struct timeval *)(optval))->tv_sec * 1000) + (((const struct timeval *)(optval))->tv_usec / 1000))
#endif


/** A struct sockaddr replacement that has the same alignment as sockaddr_in/
 *  sockaddr_in6 if instantiated.
 */
union sockaddr_aligned {
  struct sockaddr sa;
#if LWIP_IPV6
  struct sockaddr_in6 sin6;
#endif /* LWIP_IPV6 */
#if LWIP_IPV4
  struct sockaddr_in sin;
#endif /* LWIP_IPV4 */
};

/* Define the number of IPv4 multicast memberships, default is one per socket */
#ifndef LWIP_SOCKET_MAX_MEMBERSHIPS
#define LWIP_SOCKET_MAX_MEMBERSHIPS NUM_SOCKETS
#endif

#if LWIP_IGMP
/* This is to keep track of IP_ADD_MEMBERSHIP calls to drop the membership when
   a socket is closed */
struct lwip_socket_multicast_pair {
  /** the socket */
  struct lwip_sock *sock;
  /** the interface address */
  ip4_addr_t if_addr;
  /** the group address */
  ip4_addr_t multi_addr;
};

static struct lwip_socket_multicast_pair socket_ipv4_multicast_memberships[LWIP_SOCKET_MAX_MEMBERSHIPS];

static int  lwip_socket_register_membership(int s, const ip4_addr_t *if_addr, const ip4_addr_t *multi_addr);
static void lwip_socket_unregister_membership(int s, const ip4_addr_t *if_addr, const ip4_addr_t *multi_addr);
static void lwip_socket_drop_registered_memberships(int s);
#endif /* LWIP_IGMP */

#if LWIP_IPV6_MLD
/* This is to keep track of IP_JOIN_GROUP calls to drop the membership when
   a socket is closed */
struct lwip_socket_multicast_mld6_pair {
  /** the socket */
  struct lwip_sock *sock;
  /** the interface index */
  u8_t if_idx;
  /** the group address */
  ip6_addr_t multi_addr;
};

static struct lwip_socket_multicast_mld6_pair socket_ipv6_multicast_memberships[LWIP_SOCKET_MAX_MEMBERSHIPS];

static int  lwip_socket_register_mld6_membership(int s, unsigned int if_idx, const ip6_addr_t *multi_addr);
static void lwip_socket_unregister_mld6_membership(int s, unsigned int if_idx, const ip6_addr_t *multi_addr);
static void lwip_socket_drop_registered_mld6_memberships(int s);
#endif /* LWIP_IPV6_MLD */

/** The global array of available sockets */
static struct lwip_sock sockets[NUM_SOCKETS];

#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
#if LWIP_TCPIP_CORE_LOCKING
/* protect the select_cb_list using core lock */
#define LWIP_SOCKET_SELECT_DECL_PROTECT(lev)
#define LWIP_SOCKET_SELECT_PROTECT(lev)   LOCK_TCPIP_CORE()
#define LWIP_SOCKET_SELECT_UNPROTECT(lev) UNLOCK_TCPIP_CORE()
#else /* LWIP_TCPIP_CORE_LOCKING */
/* protect the select_cb_list using SYS_LIGHTWEIGHT_PROT */
#define LWIP_SOCKET_SELECT_DECL_PROTECT(lev)  SYS_ARCH_DECL_PROTECT(lev)
#define LWIP_SOCKET_SELECT_PROTECT(lev)       SYS_ARCH_PROTECT(lev)
#define LWIP_SOCKET_SELECT_UNPROTECT(lev)     SYS_ARCH_UNPROTECT(lev)
/** This counter is increased from lwip_select when the list is changed
    and checked in select_check_waiters to see if it has changed. */
static volatile int select_cb_ctr;
#endif /* LWIP_TCPIP_CORE_LOCKING */
/** The global list of tasks waiting for select */
static struct lwip_select_cb *select_cb_list;
#endif /* LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL */

#define sock_set_errno(sk, e) do { \
  const int sockerr = (e); \
  set_errno(sockerr); \
} while (0)

/* Forward declaration of some functions */
#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
static void event_callback(struct netconn *conn, enum netconn_evt evt, u16_t len);
void lwip_posix_socket_event_callback(struct lwip_sock *sock, enum netconn_evt evt, u16_t len);
#define DEFAULT_SOCKET_EVENTCB event_callback
static void select_check_waiters(int s, int has_recvevent, int has_sendevent, int has_errevent);
#else
#define DEFAULT_SOCKET_EVENTCB NULL
#endif
#if !LWIP_TCPIP_CORE_LOCKING
static void lwip_getsockopt_callback(void *arg);
static void lwip_setsockopt_callback(void *arg);
#endif
static int lwip_getsockopt_impl(int s, int level, int optname, void *optval, socklen_t *optlen);
static int lwip_setsockopt_impl(int s, int level, int optname, const void *optval, socklen_t optlen);
static int free_socket_locked(struct lwip_sock *sock, int is_tcp, struct netconn **conn,
                              union lwip_sock_lastdata *lastdata);
static void free_socket_free_elements(int is_tcp, struct netconn *conn, union lwip_sock_lastdata *lastdata);

/** LWIP_NETCONN_SEM_PER_THREAD==1: initialize thread-local semaphore */
void
lwip_socket_thread_init(void)
{
  netconn_thread_init();
}

/** LWIP_NETCONN_SEM_PER_THREAD==1: destroy thread-local semaphore */
void
lwip_socket_thread_cleanup(void)
{
  netconn_thread_cleanup();
}

#if LWIP_NETCONN_FULLDUPLEX
/* Thread-safe increment of sock->fd_used, with overflow check */
static int
sock_inc_used(struct lwip_sock *sock)
{
  int ret;
  SYS_ARCH_DECL_PROTECT(lev);

  LWIP_ASSERT("sock != NULL", sock != NULL);

  SYS_ARCH_PROTECT(lev);
  if (sock->fd_free_pending) {
    /* prevent new usage of this socket if free is pending */
    ret = 0;
  } else {
    ++sock->fd_used;
    ret = 1;
    LWIP_ASSERT("sock->fd_used != 0", sock->fd_used != 0);
  }
  SYS_ARCH_UNPROTECT(lev);
  return ret;
}

/* Like sock_inc_used(), but called under SYS_ARCH_PROTECT lock. */
static int
sock_inc_used_locked(struct lwip_sock *sock)
{
  LWIP_ASSERT("sock != NULL", sock != NULL);

  if (sock->fd_free_pending) {
    LWIP_ASSERT("sock->fd_used != 0", sock->fd_used != 0);
    return 0;
  }

  ++sock->fd_used;
  LWIP_ASSERT("sock->fd_used != 0", sock->fd_used != 0);
  return 1;
}

/* In full-duplex mode,sock->fd_used != 0 prevents a socket descriptor from being
 * released (and possibly reused) when used from more than one thread
 * (e.g. read-while-write or close-while-write, etc)
 * This function is called at the end of functions using (try)get_socket*().
 */
static void
done_socket(struct lwip_sock *sock)
{
  int freed = 0;
  int is_tcp = 0;
  struct netconn *conn = NULL;
  union lwip_sock_lastdata lastdata;
  SYS_ARCH_DECL_PROTECT(lev);
  LWIP_ASSERT("sock != NULL", sock != NULL);

  SYS_ARCH_PROTECT(lev);
  LWIP_ASSERT("sock->fd_used > 0", sock->fd_used > 0);
  if (--sock->fd_used == 0) {
    if (sock->fd_free_pending) {
      /* free the socket */
      sock->fd_used = 1;
      is_tcp = sock->fd_free_pending & LWIP_SOCK_FD_FREE_TCP;
      freed = free_socket_locked(sock, is_tcp, &conn, &lastdata);
    }
  }
  SYS_ARCH_UNPROTECT(lev);

  if (freed) {
    free_socket_free_elements(is_tcp, conn, &lastdata);
  }
}

#else /* LWIP_NETCONN_FULLDUPLEX */
#define sock_inc_used(sock)         1
#define sock_inc_used_locked(sock)  1
#define done_socket(sock)
#endif /* LWIP_NETCONN_FULLDUPLEX */

/* Translate a socket 'int' into a pointer (only fails if the index is invalid) */
static struct lwip_sock *
tryget_socket_unconn_nouse(int fd)
{
  int s = fd - LWIP_SOCKET_OFFSET;
  if ((s < 0) || (s >= NUM_SOCKETS)) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("tryget_socket_unconn(%d): invalid\n", fd));
    return NULL;
  }
  return &sockets[s];
}

struct lwip_sock *
lwip_socket_dbg_get_socket(int fd)
{
  return tryget_socket_unconn_nouse(fd);
}

/* Translate a socket 'int' into a pointer (only fails if the index is invalid) */
static struct lwip_sock *
tryget_socket_unconn(int fd)
{
  struct lwip_sock *ret = tryget_socket_unconn_nouse(fd);
  if (ret != NULL) {
    if (!sock_inc_used(ret)) {
      return NULL;
    }
  }
  return ret;
}

/* Like tryget_socket_unconn(), but called under SYS_ARCH_PROTECT lock. */
static struct lwip_sock *
tryget_socket_unconn_locked(int fd)
{
  struct lwip_sock *ret = tryget_socket_unconn_nouse(fd);
  if (ret != NULL) {
    if (!sock_inc_used_locked(ret)) {
      return NULL;
    }
  }
  return ret;
}

/**
 * Same as get_socket but doesn't set errno
 *
 * @param fd externally used socket index
 * @return struct lwip_sock for the socket or NULL if not found
 */
static struct lwip_sock *
tryget_socket(int fd)
{
  struct lwip_sock *sock = tryget_socket_unconn(fd);
  if (sock != NULL) {
    if (sock->conn) {
      return sock;
    }
    done_socket(sock);
  }
  return NULL;
}

/**
 * Map a externally used socket index to the internal socket representation.
 *
 * @param fd externally used socket index
 * @return struct lwip_sock for the socket or NULL if not found
 */
static struct lwip_sock *
get_socket(int fd)
{
  struct lwip_sock *sock = tryget_socket(fd);
  if (!sock) {
    if ((fd < LWIP_SOCKET_OFFSET) || (fd >= (LWIP_SOCKET_OFFSET + NUM_SOCKETS))) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("get_socket(%d): invalid\n", fd));
    }
    set_errno(EBADF);
    return NULL;
  }
  return sock;
}

/**
 * Allocate a new socket for a given netconn.
 *
 * @param newconn the netconn for which to allocate a socket
 * @param accepted 1 if socket has been created by accept(),
 *                 0 if socket has been created by socket()
 * @return the index of the new socket; -1 on error
 */
static int
alloc_socket(struct netconn *newconn, int accepted)
{
  int i;
  SYS_ARCH_DECL_PROTECT(lev);
  LWIP_UNUSED_ARG(accepted);

  /* allocate a new socket identifier */
  for (i = 0; i < NUM_SOCKETS; ++i) {
    /* Protect socket array */
    SYS_ARCH_PROTECT(lev);
    if (!sockets[i].conn) {
#if LWIP_NETCONN_FULLDUPLEX
      if (sockets[i].fd_used) {
        SYS_ARCH_UNPROTECT(lev);
        continue;
      }
      sockets[i].fd_used    = 1;
      sockets[i].fd_free_pending = 0;
#endif
      sockets[i].conn       = newconn;
      /* The socket is not yet known to anyone, so no need to protect
         after having marked it as used. */
      SYS_ARCH_UNPROTECT(lev);
      sockets[i].lastdata.pointer = NULL;
      sockets[i].shutdown = 0;
      sockets[i].pending_read = 0;
      sockets[i].timestamp_mode = 0;
      sockets[i].receive_original_destination = 0;
#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
      LWIP_ASSERT("sockets[i].select_waiting == 0", sockets[i].select_waiting == 0);
      sockets[i].rcvevent   = 0;
      /* TCP sendbuf is empty, but the socket is not yet writable until connected
       * (unless it has been created by accept()). */
      sockets[i].sendevent  = (NETCONNTYPE_GROUP(newconn->type) == NETCONN_TCP ? (accepted != 0) : 1);
      sockets[i].errevent   = 0;
      sockets[i].sock_data  = NULL;
#endif /* LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL */
      return i + LWIP_SOCKET_OFFSET;
    }
    SYS_ARCH_UNPROTECT(lev);
  }
  return -1;
}

/** Free a socket (under lock)
 *
 * @param sock the socket to free
 * @param is_tcp != 0 for TCP sockets, used to free lastdata
 * @param conn the socekt's netconn is stored here, must be freed externally
 * @param lastdata lastdata is stored here, must be freed externally
 */
static int
free_socket_locked(struct lwip_sock *sock, int is_tcp, struct netconn **conn,
                   union lwip_sock_lastdata *lastdata)
{
#if LWIP_NETCONN_FULLDUPLEX
  LWIP_ASSERT("sock->fd_used > 0", sock->fd_used > 0);
  sock->fd_used--;
  if (sock->fd_used > 0) {
    sock->fd_free_pending = LWIP_SOCK_FD_FREE_FREE | (is_tcp ? LWIP_SOCK_FD_FREE_TCP : 0);
    return 0;
  }
#else /* LWIP_NETCONN_FULLDUPLEX */
  LWIP_UNUSED_ARG(is_tcp);
#endif /* LWIP_NETCONN_FULLDUPLEX */

  *lastdata = sock->lastdata;
  sock->lastdata.pointer = NULL;
  *conn = sock->conn;
  sock->conn = NULL;
  return 1;
}

/** Free a socket's leftover members.
 */
static void
free_socket_free_elements(int is_tcp, struct netconn *conn, union lwip_sock_lastdata *lastdata)
{
  if (lastdata->pointer != NULL) {
    if (is_tcp) {
      struct lwip_socket_packet *packet = lastdata->tcp;

      while (packet) {
        struct lwip_socket_packet *next = packet->next;

        pbuf_free(packet->buffer);
        uk_free(uk_alloc_get_default(), packet);
        packet = next;
      }
    } else {
      netbuf_delete(lastdata->netbuf);
    }
  }
  if (conn != NULL) {
    /* netconn_prepare_delete() has already been called, here we only free the conn */
    netconn_delete(conn);
  }
}

/** Free a socket. The socket's netconn must have been
 * delete before!
 *
 * @param sock the socket to free
 * @param is_tcp != 0 for TCP sockets, used to free lastdata
 */
static void
free_socket(struct lwip_sock *sock, int is_tcp)
{
  int freed;
  struct netconn *conn;
  union lwip_sock_lastdata lastdata;
  SYS_ARCH_DECL_PROTECT(lev);

  /* Protect socket array */
  SYS_ARCH_PROTECT(lev);

  freed = free_socket_locked(sock, is_tcp, &conn, &lastdata);
  SYS_ARCH_UNPROTECT(lev);
  /* don't use 'sock' after this line, as another task might have allocated it */

  if (freed) {
    free_socket_free_elements(is_tcp, conn, &lastdata);
  }
}

/* Below this, the well-known socket functions are implemented.
 * Use google.com or opengroup.org to get a good description :-)
 *
 * Exceptions are documented!
 */

int
lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
  struct lwip_sock *sock, *nsock;
  struct netconn *newconn;
  ip_addr_t naddr;
  u16_t port = 0;
  int newsock;
  err_t err;
  int recvevent;
  SYS_ARCH_DECL_PROTECT(lev);

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_accept(%d)...\n", s));
  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  /* wait for a new connection */
  err = netconn_accept(sock->conn, &newconn);
  if (err != ERR_OK) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_accept(%d): netconn_acept failed, err=%d\n", s, err));
    if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
      sock_set_errno(sock, EOPNOTSUPP);
    } else if (err == ERR_CLSD) {
      sock_set_errno(sock, EINVAL);
    } else {
      sock_set_errno(sock, err_to_errno(err));
    }
    done_socket(sock);
    return -1;
  }
  LWIP_ASSERT("newconn != NULL", newconn != NULL);

  newsock = alloc_socket(newconn, 1);
  if (newsock == -1) {
    netconn_delete(newconn);
    sock_set_errno(sock, ENFILE);
    done_socket(sock);
    return -1;
  }
  LWIP_ASSERT("invalid socket index", (newsock >= LWIP_SOCKET_OFFSET) && (newsock < NUM_SOCKETS + LWIP_SOCKET_OFFSET));
  nsock = &sockets[newsock - LWIP_SOCKET_OFFSET];

  /* See event_callback: If data comes in right away after an accept, even
   * though the server task might not have created a new socket yet.
   * In that case, newconn->socket is counted down (newconn->socket--),
   * so nsock->rcvevent is >= 1 here!
   */
  SYS_ARCH_PROTECT(lev);
  recvevent = (s16_t)(-1 - newconn->socket);
  newconn->socket = newsock;
  SYS_ARCH_UNPROTECT(lev);

  if (newconn->callback) {
    LOCK_TCPIP_CORE();
    while (recvevent > 0) {
      recvevent--;
      newconn->callback(newconn, NETCONN_EVT_RCVPLUS, 0);
    }
    UNLOCK_TCPIP_CORE();
  }

  /* Note that POSIX only requires us to check addr is non-NULL. addrlen must
   * not be NULL if addr is valid.
   */
  if ((addr != NULL) && (addrlen != NULL)) {
    union sockaddr_aligned tempaddr;
    /* get the IP address and port of the remote host */
    err = netconn_peer(newconn, &naddr, &port);
    if (err != ERR_OK) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_accept(%d): netconn_peer failed, err=%d\n", s, err));
      free_socket(nsock, 1);
      sock_set_errno(sock, err_to_errno(err));
      done_socket(sock);
      return -1;
    }

    IPADDR_PORT_TO_SOCKADDR(&tempaddr, &naddr, port);
    if (*addrlen > sizeof(*addr)) {
      *addrlen = sizeof(*addr);
    }
    MEMCPY(addr, &tempaddr, *addrlen);

    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_accept(%d) returning new sock=%d addr=", s, newsock));
    ip_addr_debug_print_val(SOCKETS_DEBUG, naddr);
    LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F"\n", port));
  } else {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_accept(%d) returning new sock=%d", s, newsock));
  }

  sock_set_errno(sock, 0);
  done_socket(sock);
  done_socket(nsock);
  return newsock;
}

static int socket_address_decode(struct lwip_sock *sock,
                                 const struct sockaddr *name, socklen_t length,
                                 u8_t binding, ip_addr_t *address, u16_t *port)
{
  LWIP_UNUSED_ARG(binding);
  if (!IS_SOCK_ADDR_ALIGNED(name)) {
    return EINVAL;
  }
#if LWIP_IPV4
  if (!NETCONNTYPE_ISIPV6(netconn_type(sock->conn))) {
    const struct sockaddr_in *input = (const struct sockaddr_in *)name;

    if (length < sizeof(*input)) {
      return EINVAL;
    }
    if (name->sa_family != AF_INET &&
        (name->sa_family != AF_UNSPEC ||
         (binding && input->sin_addr.s_addr != INADDR_ANY))) {
      return EAFNOSUPPORT;
    }
    SOCKADDR4_TO_IP4ADDR_PORT(input, address, *port);
    IP_SET_TYPE_VAL(*address, IPADDR_TYPE_V4);
    return 0;
  }
#endif /* LWIP_IPV4 */
#if LWIP_IPV6
  {
    struct sockaddr_in6 input = {0};

    if (length < offsetof(struct sockaddr_in6, sin6_scope_id)) {
      return EINVAL;
    }
    memcpy(&input, name, length >= sizeof(input) ? sizeof(input) :
           offsetof(struct sockaddr_in6, sin6_scope_id));
    if (input.sin6_family != AF_INET6) {
      return EAFNOSUPPORT;
    }
    SOCKADDR6_TO_IP6ADDR_PORT(&input, address, *port);
    IP_SET_TYPE_VAL(*address, IPADDR_TYPE_V6);
    return 0;
  }
#else  /* LWIP_IPV6 */
  return EAFNOSUPPORT;
#endif /* LWIP_IPV6 */
}

#if LWIP_UDP
struct socket_datagram_admission {
  struct tcpip_api_call_data call;
  struct netconn *connection;
  u8_t named;
  u8_t disconnect;
  int error;
};

static err_t socket_udp_admit(struct tcpip_api_call_data *call)
{
  struct socket_datagram_admission *admission =
    (struct socket_datagram_admission *)call;
  struct udp_pcb *pcb = admission->connection->pcb.udp;
  err_t error;

  if (!pcb) {
    return ERR_CLSD;
  }
  if (admission->disconnect) {
    udp_disconnect(pcb);
    if (!admission->connection->socket_udp_port_locked) {
      udp_unbind(pcb);
    }
    return ERR_OK;
  }
  error = pcb->local_port ? ERR_OK : udp_bind(pcb, &pcb->local_ip, 0);
  if (error == ERR_OK && !admission->named &&
      !(pcb->flags & UDP_FLAGS_CONNECTED)) {
    admission->error = EDESTADDRREQ;
  }
  return error;
}

static int socket_datagram_prepare(struct lwip_sock *sock, u8_t named,
                                   u8_t disconnect)
{
  struct socket_datagram_admission admission = {
    .connection = sock->conn, .named = named, .disconnect = disconnect};
  err_t error;

  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_UDP) {
    return 0;
  }
  error = tcpip_api_call(socket_udp_admit, &admission.call);
  return error == ERR_OK ? admission.error : err_to_errno(error);
}
#endif /* LWIP_UDP */

int
lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
  struct lwip_sock *sock;
  ip_addr_t local_addr;
  u16_t local_port;
  err_t err;

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  {
    int error =
      socket_address_decode(sock, name, namelen, 1, &local_addr, &local_port);
    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
  }
  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_bind(%d, addr=", s));
  ip_addr_debug_print_val(SOCKETS_DEBUG, local_addr);
  LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F")\n", local_port));

#if LWIP_IPV4 && LWIP_IPV6
  /* Dual-stack: Unmap IPv4 mapped IPv6 addresses */
  if (IP_IS_V6_VAL(local_addr) && ip6_addr_isipv4mappedipv6(ip_2_ip6(&local_addr))) {
    unmap_ipv4_mapped_ipv6(ip_2_ip4(&local_addr), ip_2_ip6(&local_addr));
    IP_SET_TYPE_VAL(local_addr, IPADDR_TYPE_V4);
  }
#endif /* LWIP_IPV4 && LWIP_IPV6 */

  err = netconn_bind(sock->conn, &local_addr, local_port);

  if (err != ERR_OK) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_bind(%d) failed, err=%d\n", s, err));
    sock_set_errno(sock, err_to_errno(err));
    done_socket(sock);
    return -1;
  }

#if LWIP_UDP
  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_UDP) {
    sock->conn->socket_udp_port_locked = local_port != 0;
  }
#endif /* LWIP_UDP */
  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_bind(%d) succeeded\n", s));
  sock_set_errno(sock, 0);
  done_socket(sock);
  return 0;
}

int
lwip_close(int s)
{
  struct lwip_sock *sock;
  int is_tcp = 0;
  err_t err;

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_close(%d)\n", s));

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  if (sock->conn != NULL) {
    is_tcp = NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP;
  } else {
    LWIP_ASSERT("sock->lastdata == NULL", sock->lastdata.pointer == NULL);
  }

#if LWIP_IGMP
  /* drop all possibly joined IGMP memberships */
  lwip_socket_drop_registered_memberships(s);
#endif /* LWIP_IGMP */
#if LWIP_IPV6_MLD
  /* drop all possibly joined MLD6 memberships */
  lwip_socket_drop_registered_mld6_memberships(s);
#endif /* LWIP_IPV6_MLD */

  err = netconn_prepare_delete(sock->conn);
  if (err != ERR_OK) {
    sock_set_errno(sock, err_to_errno(err));
    done_socket(sock);
    return -1;
  }

  free_socket(sock, is_tcp);
  set_errno(0);
  return 0;
}

int
lwip_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
  struct lwip_sock *sock;
  err_t err;

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  if (namelen < sizeof(name->sa_family)) {
    sock_set_errno(sock, EINVAL);
    done_socket(sock);
    return -1;
  }
  if (name->sa_family == AF_UNSPEC) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_connect(%d, AF_UNSPEC)\n", s));
#if LWIP_UDP
    if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_UDP) {
      int error = socket_datagram_prepare(sock, 1, 1);

      sock_set_errno(sock, error);
      done_socket(sock);
      return error ? -1 : 0;
    }
#endif /* LWIP_UDP */
    err = netconn_disconnect(sock->conn);
  } else {
    ip_addr_t remote_addr;
    u16_t remote_port;
    int error;

#if LWIP_UDP
    error = socket_datagram_prepare(sock, 1, 0);
    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
#endif /* LWIP_UDP */
    error =
      socket_address_decode(sock, name, namelen, 0, &remote_addr, &remote_port);
    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_connect(%d, addr=", s));
    ip_addr_debug_print_val(SOCKETS_DEBUG, remote_addr);
    LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F")\n", remote_port));

#if LWIP_IPV4 && LWIP_IPV6
    /* Dual-stack: Unmap IPv4 mapped IPv6 addresses */
    if (IP_IS_V6_VAL(remote_addr) && ip6_addr_isipv4mappedipv6(ip_2_ip6(&remote_addr))) {
      unmap_ipv4_mapped_ipv6(ip_2_ip4(&remote_addr), ip_2_ip6(&remote_addr));
      IP_SET_TYPE_VAL(remote_addr, IPADDR_TYPE_V4);
    }
#endif /* LWIP_IPV4 && LWIP_IPV6 */

    err = netconn_connect(sock->conn, &remote_addr, remote_port);
  }

  if (err != ERR_OK) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_connect(%d) failed, err=%d\n", s, err));
    sock_set_errno(sock, err_to_errno(err));
    done_socket(sock);
    return -1;
  }

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_connect(%d) succeeded\n", s));
  sock_set_errno(sock, 0);
  done_socket(sock);
  return 0;
}

/**
 * Set a socket into listen mode.
 * The socket may not have been used for another connection previously.
 *
 * @param s the socket to set to listening mode
 * @param backlog (ATTENTION: needs TCP_LISTEN_BACKLOG=1)
 * @return 0 on success, non-zero on failure
 */
int
lwip_listen(int s, int backlog)
{
  struct lwip_sock *sock;
  err_t err;

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_listen(%d, backlog=%d)\n", s, backlog));

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  /* limit the "backlog" parameter to fit in an u8_t */
  backlog = LWIP_MIN(LWIP_MAX(backlog, 0), 0xff);

  err = netconn_listen_with_backlog(sock->conn, (u8_t)backlog);

  if (err != ERR_OK) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_listen(%d) failed, err=%d\n", s, err));
    if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
      sock_set_errno(sock, EOPNOTSUPP);
    } else {
      sock_set_errno(sock, err_to_errno(err));
    }
    done_socket(sock);
    return -1;
  }

  sock_set_errno(sock, 0);
  done_socket(sock);
  return 0;
}

struct socket_iov_cursor {
  const struct iovec *vectors;
  size_t count;
  size_t index;
  size_t offset;
};

static int
socket_iov_size(const struct iovec *vectors, size_t count, size_t *length)
{
  *length = 0;
  if (count > IOV_MAX) {
    return EMSGSIZE;
  }
  if (count && !vectors) {
    return EFAULT;
  }
  for (size_t i = 0; i < count; ++i) {
    if (vectors[i].iov_len > (size_t)SSIZE_MAX - *length) {
      return EINVAL;
    }
    *length += vectors[i].iov_len;
  }
  return 0;
}

static size_t
socket_iov_copy(struct socket_iov_cursor *cursor, void *memory, size_t length,
                int sending)
{
  size_t done = 0;

  while (done < length && cursor->index < cursor->count) {
    const struct iovec *vector = &cursor->vectors[cursor->index];
    size_t bytes = LWIP_MIN(vector->iov_len - cursor->offset, length - done);
    __uptr address = (__uptr)vector->iov_base + cursor->offset;
    char *user = (char *)address;
    char *kernel = (char *)memory + done;
    size_t copied;

    if (address < (__uptr)vector->iov_base) {
      break;
    }
    copied = uk_nofault_memcpy(sending ? kernel : user,
                               sending ? user : kernel, bytes, 0);
    done += copied;
    cursor->offset += copied;
    if (copied != bytes) {
      break;
    }
    if (cursor->offset == vector->iov_len) {
      cursor->index++;
      cursor->offset = 0;
    }
  }
  return done;
}

static size_t
socket_pbuf_copy(const struct pbuf *packet, size_t offset, size_t length,
                 struct socket_iov_cursor *cursor)
{
  size_t done = 0;

  while (packet && offset >= packet->len) {
    offset -= packet->len;
    packet = packet->next;
  }
  while (packet && done < length) {
    size_t bytes = LWIP_MIN((size_t)packet->len - offset, length - done);
    size_t copied = socket_iov_copy(cursor, (char *)packet->payload + offset,
                                    bytes, 0);
    done += copied;
    if (copied != bytes) {
      break;
    }
    offset = 0;
    packet = packet->next;
  }
  return done;
}

#if LWIP_TCP
struct socket_tcp_admission {
  struct tcpip_api_call_data call;
  struct netconn *connection;
  u8_t receive;
  u16_t requested;
  u16_t available;
};

static err_t
socket_tcp_admit(struct tcpip_api_call_data *call)
{
  struct socket_tcp_admission *admission = (struct socket_tcp_admission *)call;
  struct netconn *connection = admission->connection;
  err_t error;

  if (admission->receive) {
    struct tcp_pcb *pcb = connection->pcb.tcp;

    return pcb && (pcb->state == CLOSED || pcb->state == LISTEN) ?
      ERR_CONN : ERR_OK;
  }
  error = netconn_err(connection);

  if (error != ERR_OK) {
    return error;
  }
  if (connection->state != NETCONN_NONE) {
    return ERR_INPROGRESS;
  }
  if (!connection->pcb.tcp) {
    return ERR_CONN;
  }
  error = tcp_write_capacity(connection->pcb.tcp, admission->requested,
                             &admission->available);
  if (error == ERR_MEM) {
    API_EVENT(connection, NETCONN_EVT_SENDMINUS, 0);
    connection->flags |= NETCONN_FLAG_CHECK_WRITESPACE;
    return ERR_WOULDBLOCK;
  }
  return error;
}

static ssize_t
lwip_recv_tcp_vectors(struct lwip_sock *sock, const struct iovec *vectors,
                      size_t count, size_t length, int flags)
{
  struct socket_iov_cursor cursor = {.vectors = vectors, .count = count};
  struct lwip_socket_packet **slot = &sock->lastdata.tcp;
  u8_t apiflags = NETCONN_NOAUTORCVD;
  size_t recvd = 0;
  int error = 0;
  struct socket_tcp_admission admission = {
    .connection = sock->conn, .receive = 1
  };
  err_t admitted;
  SYS_ARCH_DECL_PROTECT(lev);

  admitted = tcpip_api_call(socket_tcp_admit, &admission.call);
  if (admitted != ERR_OK) {
    sock_set_errno(sock, err_to_errno(admitted));
    return -1;
  }

  if (flags & MSG_DONTWAIT) {
    apiflags |= NETCONN_DONTBLOCK;
  }
  while (recvd < length || !*slot) {
    struct lwip_socket_packet *packet = *slot;
    size_t bytes;

    if (!packet) {
      err_t err;

      packet = uk_malloc(uk_alloc_get_default(), sizeof(*packet));
      if (!packet) {
        error = ENOMEM;
        break;
      }
      err = netconn_recv_tcp_pbuf_flags(sock->conn, &packet->buffer, apiflags);
      if (err != ERR_OK) {
        uk_free(uk_alloc_get_default(), packet);
        error = err == ERR_CLSD ? 0 : err_to_errno(err);
        break;
      }
      packet->next = NULL;
      SYS_ARCH_PROTECT(lev);
      *slot = packet;
      SYS_ARCH_UNPROTECT(lev);
    }
    bytes = LWIP_MIN(length - recvd, (size_t)packet->buffer->tot_len);
    if (socket_pbuf_copy(packet->buffer, 0, bytes, &cursor) != bytes) {
      error = EFAULT;
      break;
    }
    recvd += bytes;
    if (flags & MSG_PEEK) {
      slot = &packet->next;
    } else {
      packet->buffer = pbuf_free_header(packet->buffer, (u16_t)bytes);
      if (!packet->buffer) {
        SYS_ARCH_PROTECT(lev);
        *slot = packet->next;
        SYS_ARCH_UNPROTECT(lev);
        uk_free(uk_alloc_get_default(), packet);
      }
    }
    apiflags |= NETCONN_DONTBLOCK | NETCONN_NOFIN;
  }
  if (recvd && !(flags & MSG_PEEK)) {
    netconn_tcp_recvd(sock->conn, recvd);
  }
  sock_set_errno(sock, recvd ? 0 : error);
  return recvd ? (ssize_t)recvd : error ? -1 : 0;
}

static ssize_t
lwip_recv_tcp(struct lwip_sock *sock, void *memory, size_t length, int flags)
{
  struct iovec vector = {.iov_base = memory, .iov_len = length};

  return lwip_recv_tcp_vectors(sock, &vector, 1,
                               LWIP_MIN(length, (size_t)SSIZE_MAX), flags);
}

static ssize_t
lwip_send_tcp_vectors(struct lwip_sock *sock, const struct iovec *vectors,
                      size_t count, size_t length, int flags)
{
  struct socket_iov_cursor cursor = {.vectors = vectors, .count = count};
  struct socket_tcp_admission admission = {.connection = sock->conn};
  size_t capacity = LWIP_MIN(length, (size_t)0xffff);
  size_t sent = 0;
  void *memory = NULL;
  int error = 0;

  do {
    size_t bytes;
    size_t written = 0;
    u8_t options = NETCONN_COPY | NETCONN_DONTBLOCK;
    err_t err;

    admission.requested = (u16_t)LWIP_MIN(capacity, length - sent);
    err = tcpip_api_call(socket_tcp_admit, &admission.call);
    if (err != ERR_OK) {
      error = err == ERR_CONN ? EPIPE : err_to_errno(err);
      break;
    }
    if (!length) {
      break;
    }
    bytes = admission.available;
    if (!memory) {
      memory = uk_malloc(uk_alloc_get_default(), capacity);
      if (!memory) {
        error = ENOMEM;
        break;
      }
    }
    if (socket_iov_copy(&cursor, memory, bytes, 1) != bytes) {
      error = EFAULT;
      break;
    }
    if ((flags & MSG_MORE) || sent + bytes < length) {
      options |= NETCONN_MORE;
    }
    err = netconn_write_partly(sock->conn, memory, bytes, options, &written);
    sent += written;
    if (err != ERR_OK || written != bytes) {
      error = err == ERR_CONN ? EPIPE : err_to_errno(err);
      break;
    }
  } while (sent < length);
  uk_free(uk_alloc_get_default(), memory);
  sock_set_errno(sock, sent ? 0 : error);
  return sent ? (ssize_t)sent : error ? -1 : 0;
}

#endif

/* Convert a netbuf's address data to struct sockaddr */
static int
lwip_sock_make_addr(struct netconn *conn, ip_addr_t *fromaddr, u16_t port,
                    struct sockaddr *from, socklen_t *fromlen)
{
  int truncated = 0;
  union sockaddr_aligned saddr;

  LWIP_UNUSED_ARG(conn);

  LWIP_ASSERT("fromaddr != NULL", fromaddr != NULL);
  LWIP_ASSERT("from != NULL", from != NULL);
  LWIP_ASSERT("fromlen != NULL", fromlen != NULL);

#if LWIP_IPV4 && LWIP_IPV6
  /* Dual-stack: Map IPv4 addresses to IPv4 mapped IPv6 */
  if (NETCONNTYPE_ISIPV6(netconn_type(conn)) && IP_IS_V4(fromaddr)) {
    ip4_2_ipv4_mapped_ipv6(ip_2_ip6(fromaddr), ip_2_ip4(fromaddr));
    IP_SET_TYPE(fromaddr, IPADDR_TYPE_V6);
  }
#endif /* LWIP_IPV4 && LWIP_IPV6 */

  IPADDR_PORT_TO_SOCKADDR(&saddr, fromaddr, port);
  if (*fromlen < sizeof(*from)) {
    truncated = 1;
  } else {
    *fromlen = sizeof(*from);
  }
  MEMCPY(from, &saddr, *fromlen);
  return truncated;
}

#if LWIP_TCP
/* Helper function to get a tcp socket's remote address info */
static int
lwip_recv_tcp_from(struct lwip_sock *sock, struct sockaddr *from, socklen_t *fromlen, const char *dbg_fn, int dbg_s, ssize_t dbg_ret)
{
  if (sock == NULL) {
    return 0;
  }
  LWIP_UNUSED_ARG(dbg_fn);
  LWIP_UNUSED_ARG(dbg_s);
  LWIP_UNUSED_ARG(dbg_ret);

#if !SOCKETS_DEBUG
  if (from && fromlen)
#endif /* !SOCKETS_DEBUG */
  {
    /* get remote addr/port from tcp_pcb */
    u16_t port;
    ip_addr_t tmpaddr;
    netconn_getaddr(sock->conn, &tmpaddr, &port, 0);
    LWIP_DEBUGF(SOCKETS_DEBUG, ("%s(%d):  addr=", dbg_fn, dbg_s));
    ip_addr_debug_print_val(SOCKETS_DEBUG, tmpaddr);
    LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F" len=%d\n", port, (int)dbg_ret));
    if (from && fromlen) {
      return lwip_sock_make_addr(sock->conn, &tmpaddr, port, from, fromlen);
    }
  }
  return 0;
}
#endif

static int
socket_cmsg_append(struct msghdr *message, size_t capacity, int level,
                   int type, const void *data, size_t size)
{
  size_t available = capacity - message->msg_controllen;
  struct cmsghdr header = {
    .cmsg_level = level, .cmsg_type = type,
    .cmsg_len = LWIP_MIN(available, CMSG_LEN(size))
  };
  char *output = (char *)message->msg_control + message->msg_controllen;
  size_t payload;

  if (available < CMSG_LEN(size)) {
    message->msg_flags |= MSG_CTRUNC;
  }
  if (available < CMSG_LEN(0)) {
    return 1;
  }
  payload = header.cmsg_len - CMSG_LEN(0);
  if (uk_nofault_memcpy(output, (const char *)&header, sizeof(header), 0) != sizeof(header) ||
      uk_nofault_memcpy(output + CMSG_LEN(0), data, payload, 0) != payload) {
    return 0;
  }
  message->msg_controllen += LWIP_MIN(available, CMSG_SPACE(size));
  return 1;
}

/* Helper function to receive a netbuf from a udp or raw netconn.
 * Keeps sock->lastdata for peeking.
 */
static err_t
lwip_recvfrom_udp_raw(struct lwip_sock *sock, int flags, struct msghdr *msg, u16_t *datagram_len, int dbg_s)
{
  struct netbuf *buf;
  u8_t apiflags;
  err_t err;
  u16_t buflen;
  size_t copied, capacity;
  struct socket_iov_cursor cursor = {
    .vectors = msg->msg_iov, .count = msg->msg_iovlen
  };

  LWIP_UNUSED_ARG(dbg_s);
  LWIP_ERROR("lwip_recvfrom_udp_raw: invalid arguments", (msg->msg_iov != NULL) || (msg->msg_iovlen <= 0), return ERR_ARG;);

  if (flags & MSG_DONTWAIT) {
    apiflags = NETCONN_DONTBLOCK;
  } else {
    apiflags = 0;
  }

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom_udp_raw[UDP/RAW]: top sock->lastdata=%p\n", (void *)sock->lastdata.netbuf));
  /* Check if there is data left from the last recv operation. */
  buf = sock->lastdata.netbuf;
  if (buf == NULL) {
    /* No data was left from the previous operation, so we try to get
        some from the network. */
    err = netconn_recv_udp_raw_netbuf_flags(sock->conn, &buf, apiflags);
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom_udp_raw[UDP/RAW]: netconn_recv err=%d, netbuf=%p\n",
                                err, (void *)buf));

    if (err != ERR_OK) {
      if (err == ERR_WOULDBLOCK && !(flags & MSG_DONTWAIT) &&
          (uk_load_n(&sock->shutdown) & LWIP_SOCKET_SHUT_RD)) {
        msg->msg_namelen = 0;
        msg->msg_controllen = 0;
        msg->msg_flags = 0;
        if (datagram_len) {
          *datagram_len = 0;
        }
        return ERR_OK;
      }
      return err;
    }
    LWIP_ASSERT("buf != NULL", buf != NULL);
    sock->lastdata.netbuf = buf;
  }
  buflen = buf->p->tot_len;
  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom_udp_raw: buflen=%"U16_F"\n", buflen));

  if (socket_iov_size(msg->msg_iov, msg->msg_iovlen, &capacity)) {
    return ERR_VAL;
  }
  capacity = LWIP_MIN(capacity, (size_t)buflen);
  copied = socket_pbuf_copy(buf->p, 0, capacity, &cursor);
  if (copied != capacity) {
    if (!(flags & MSG_PEEK)) {
      sock->lastdata.netbuf = NULL;
      netbuf_delete(buf);
    }
    return ERR_FAULT;
  }

  /* Check to see from where the data was.*/
#if !SOCKETS_DEBUG
  if (msg->msg_name && msg->msg_namelen)
#endif /* !SOCKETS_DEBUG */
  {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom_udp_raw(%d):  addr=", dbg_s));
    ip_addr_debug_print_val(SOCKETS_DEBUG, *netbuf_fromaddr(buf));
    LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F" len=%d\n", netbuf_fromport(buf), copied));
    if (msg->msg_name && msg->msg_namelen) {
      lwip_sock_make_addr(sock->conn, netbuf_fromaddr(buf), netbuf_fromport(buf),
                          (struct sockaddr *)msg->msg_name, &msg->msg_namelen);
    }
  }

  /* Initialize flag output */
  msg->msg_flags = 0;

  {
    size_t capacity = msg->msg_control ? msg->msg_controllen : 0;
    unsigned int timestamp_mode = uk_load_n(&sock->timestamp_mode);

    msg->msg_controllen = 0;
    if (timestamp_mode) {
      struct timespec stamp = buf->timestamp;

      if (timestamp_mode == SO_TIMESTAMP || timestamp_mode == SO_TIMESTAMP_NEW) {
        stamp.tv_nsec /= 1000;
      }
      if (!socket_cmsg_append(msg, capacity, SOL_SOCKET, timestamp_mode,
                              &stamp, sizeof(stamp))) {
        goto control_done;
      }
    }
#if LWIP_NETBUF_RECVINFO && LWIP_IPV4
    if ((buf->flags & NETBUF_FLAG_DESTADDR) && IP_IS_V4(&buf->toaddr)) {
      if (uk_load_n(&sock->conn->flags) & NETCONN_FLAG_PKTINFO) {
        struct in_pktinfo info = {0};

        info.ipi_ifindex = buf->p->if_idx;
        inet_addr_from_ip4addr(&info.ipi_addr, ip_2_ip4(&buf->toaddr));
        inet_addr_from_ip4addr(&info.ipi_spec_dst, &buf->spec_dst);
        if (!socket_cmsg_append(msg, capacity, IPPROTO_IP, IP_PKTINFO,
                                &info, sizeof(info))) {
          goto control_done;
        }
      }
      if (uk_load_n(&sock->receive_original_destination)) {
        struct sockaddr_in destination = {0};

        destination.sin_family = AF_INET;
        destination.sin_port = buf->toport_chksum;
        inet_addr_from_ip4addr(&destination.sin_addr, ip_2_ip4(&buf->toaddr));
        if (!socket_cmsg_append(msg, capacity, IPPROTO_IP, IP_ORIGDSTADDR,
                                &destination, sizeof(destination))) {
          goto control_done;
        }
      }
    }
#endif
  }
control_done:

  /* If we don't peek the incoming message: zero lastdata pointer and free the netbuf */
  if ((flags & MSG_PEEK) == 0) {
    sock->lastdata.netbuf = NULL;
    netbuf_delete(buf);
  }
  if (datagram_len) {
    *datagram_len = buflen;
  }
  return ERR_OK;
}

ssize_t
lwip_recvfrom(int s, void *mem, size_t len, int flags,
              struct sockaddr *from, socklen_t *fromlen)
{
  struct lwip_sock *sock;
  ssize_t ret;

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom(%d, %p, %"SZT_F", 0x%x, ..)\n", s, mem, len, flags));
  sock = get_socket(s);
  if (!sock) {
    return -1;
  }
#if LWIP_TCP
  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
    ret = lwip_recv_tcp(sock, mem, len, flags);
    lwip_recv_tcp_from(sock, from, fromlen, "lwip_recvfrom", s, ret);
    done_socket(sock);
    return ret;
  } else
#endif
  {
    u16_t datagram_len = 0;
    struct iovec vec;
    struct msghdr msg;
    err_t err;
    vec.iov_base = mem;
    vec.iov_len = len;
    msg.msg_control = NULL;
    msg.msg_controllen = 0;
    msg.msg_flags = 0;
    msg.msg_iov = &vec;
    msg.msg_iovlen = 1;
    msg.msg_name = from;
    msg.msg_namelen = (fromlen ? *fromlen : 0);
    err = lwip_recvfrom_udp_raw(sock, flags, &msg, &datagram_len, s);
    if (err != ERR_OK) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvfrom[UDP/RAW](%d): buf == NULL, error is \"%s\"!\n",
                                  s, lwip_strerr(err)));
      sock_set_errno(sock, err_to_errno(err));
      done_socket(sock);
      return -1;
    }
    ret = (flags & MSG_TRUNC) ? datagram_len :
      (ssize_t)LWIP_MIN(LWIP_MIN(len, datagram_len), SSIZE_MAX);
    if (fromlen) {
      *fromlen = msg.msg_namelen;
    }
  }

  sock_set_errno(sock, 0);
  done_socket(sock);
  return ret;
}

ssize_t
lwip_read(int s, void *mem, size_t len)
{
  return lwip_recvfrom(s, mem, len, 0, NULL, NULL);
}

ssize_t
lwip_readv(int s, const struct iovec *iov, int iovcnt)
{
  struct msghdr msg;

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  /* Hack: we have to cast via number to cast from 'const' pointer to non-const.
     Blame the opengroup standard for this inconsistency. */
  msg.msg_iov = LWIP_CONST_CAST(struct iovec *, iov);
  msg.msg_iovlen = iovcnt;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return lwip_recvmsg(s, &msg, 0);
}

ssize_t
lwip_recv(int s, void *mem, size_t len, int flags)
{
  return lwip_recvfrom(s, mem, len, flags, NULL, NULL);
}

ssize_t
lwip_recvmsg(int s, struct msghdr *message, int flags)
{
  struct lwip_sock *sock;
  size_t capacity;
  int error;

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvmsg(%d, message=%p, flags=0x%x)\n", s, (void *)message, flags));
  LWIP_ERROR("lwip_recvmsg: invalid message pointer", message != NULL, return ERR_ARG;);
  LWIP_ERROR("lwip_recvmsg: unsupported flags",
             (flags & ~(MSG_PEEK | MSG_DONTWAIT | MSG_TRUNC)) == 0,
             set_errno(EOPNOTSUPP); return -1;);

  error = socket_iov_size(message->msg_iov, message->msg_iovlen, &capacity);
  if (error) {
    set_errno(error);
    return -1;
  }

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
#if LWIP_TCP
    ssize_t buflen = lwip_recv_tcp_vectors(sock, message->msg_iov,
                                          message->msg_iovlen, capacity, flags);
    if (buflen >= 0) {
      message->msg_flags = 0;
      message->msg_controllen = 0;
    }
    done_socket(sock);
    return buflen;
#else /* LWIP_TCP */
    sock_set_errno(sock, err_to_errno(ERR_ARG));
    done_socket(sock);
    return -1;
#endif /* LWIP_TCP */
  }
  /* else, UDP and RAW NETCONNs */
#if LWIP_UDP || LWIP_RAW
  {
    u16_t datagram_len = 0;
    err_t err;
    err = lwip_recvfrom_udp_raw(sock, flags, message, &datagram_len, s);
    if (err != ERR_OK) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_recvmsg[UDP/RAW](%d): buf == NULL, error is \"%s\"!\n",
                                  s, lwip_strerr(err)));
      sock_set_errno(sock, err_to_errno(err));
      done_socket(sock);
      return -1;
    }
    if (datagram_len > capacity) {
      message->msg_flags |= MSG_TRUNC;
    }

    sock_set_errno(sock, 0);
    done_socket(sock);
    return (flags & MSG_TRUNC) ? datagram_len :
      (ssize_t)LWIP_MIN((size_t)datagram_len, capacity);
  }
#else /* LWIP_UDP || LWIP_RAW */
  sock_set_errno(sock, err_to_errno(ERR_ARG));
  done_socket(sock);
  return -1;
#endif /* LWIP_UDP || LWIP_RAW */
}

static int socket_datagram_destination(struct lwip_sock *sock,
                                       const struct sockaddr *name,
                                       socklen_t length, struct netbuf *buffer)
{
  u16_t port = 0;
  int error;

  if (!name) {
    ip_addr_set_any(NETCONNTYPE_ISIPV6(netconn_type(sock->conn)),
                    &buffer->addr);
  } else {
    error = socket_address_decode(sock, name, length, 0, &buffer->addr, &port);
    if (error) {
      return error;
    }
    if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_UDP && !port) {
      return EINVAL;
    }
  }
  netbuf_fromport(buffer) = port;
  return 0;
}

static err_t
socket_send_datagram(struct lwip_sock *sock, struct netbuf *buffer)
{
#if LWIP_IPV4 && LWIP_IPV6
  if (IP_IS_V6_VAL(buffer->addr) &&
      ip6_addr_isipv4mappedipv6(ip_2_ip6(&buffer->addr))) {
    unmap_ipv4_mapped_ipv6(ip_2_ip4(&buffer->addr), ip_2_ip6(&buffer->addr));
    IP_SET_TYPE_VAL(buffer->addr, IPADDR_TYPE_V4);
  }
#endif /* LWIP_IPV4 && LWIP_IPV6 */
  return netconn_send(sock->conn, buffer);
}

ssize_t
lwip_send(int s, const void *data, size_t size, int flags)
{
  struct lwip_sock *sock;
#if LWIP_TCP
  struct iovec vector = {.iov_base = (void *)data, .iov_len = size};
  ssize_t result;
#endif

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_send(%d, data=%p, size=%"SZT_F", flags=0x%x)\n",
                              s, data, size, flags));

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
#if (LWIP_UDP || LWIP_RAW)
    done_socket(sock);
    return lwip_sendto(s, data, size, flags, NULL, 0);
#else /* (LWIP_UDP || LWIP_RAW) */
    sock_set_errno(sock, err_to_errno(ERR_ARG));
    done_socket(sock);
    return -1;
#endif /* (LWIP_UDP || LWIP_RAW) */
  }

#if LWIP_TCP
  result = lwip_send_tcp_vectors(sock, &vector, 1,
                                 LWIP_MIN(size, (size_t)SSIZE_MAX), flags);
  done_socket(sock);
  return result;
#else
  sock_set_errno(sock, EOPNOTSUPP);
  done_socket(sock);
  return -1;
#endif
}

ssize_t
lwip_sendmsg(int s, const struct msghdr *msg, int flags)
{
  struct lwip_sock *sock;
  size_t capacity;
  int error;

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  LWIP_ERROR("lwip_sendmsg: invalid msghdr", msg != NULL,
             sock_set_errno(sock, err_to_errno(ERR_ARG)); done_socket(sock); return -1;);
  error = socket_iov_size(msg->msg_iov, msg->msg_iovlen, &capacity);
  if (error) {
    sock_set_errno(sock, error);
    done_socket(sock);
    return -1;
  }
#if LWIP_UDP
  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
    error = socket_datagram_prepare(sock, msg->msg_name != NULL, 0);
    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
  }
#endif /* LWIP_UDP */
  LWIP_ERROR("lwip_sendmsg: unsupported flags", (flags & ~(MSG_DONTWAIT | MSG_MORE)) == 0,
             sock_set_errno(sock, EOPNOTSUPP); done_socket(sock); return -1;);

  LWIP_UNUSED_ARG(msg->msg_control);
  LWIP_UNUSED_ARG(msg->msg_controllen);
  LWIP_UNUSED_ARG(msg->msg_flags);

  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
#if LWIP_TCP
    ssize_t result = lwip_send_tcp_vectors(sock, msg->msg_iov, msg->msg_iovlen,
                                           capacity, flags);
    done_socket(sock);
    return result;
#else /* LWIP_TCP */
    sock_set_errno(sock, err_to_errno(ERR_ARG));
    done_socket(sock);
    return -1;
#endif /* LWIP_TCP */
  }
  /* else, UDP and RAW NETCONNs */
#if LWIP_UDP || LWIP_RAW
  {
    struct netbuf chain_buf;
    err_t err = ERR_OK;
    ssize_t size = 0;

    LWIP_UNUSED_ARG(flags);
    memset(&chain_buf, 0, sizeof(struct netbuf));
    if (capacity > 0xffff) {
      goto sendmsg_emsgsize;
    }
    error = socket_datagram_destination(sock, msg->msg_name, msg->msg_namelen,
                                        &chain_buf);
    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
    if (uk_load_n(&sock->shutdown) & LWIP_SOCKET_SHUT_WR) {
      sock_set_errno(sock, EPIPE);
      done_socket(sock);
      return -1;
    }
    size = (ssize_t)capacity;
    if (netbuf_alloc(&chain_buf, (u16_t)capacity) == NULL) {
      err = ERR_MEM;
    } else {
      struct socket_iov_cursor cursor = {
        .vectors = msg->msg_iov, .count = msg->msg_iovlen
      };

      if (socket_iov_copy(&cursor, chain_buf.p->payload,
                          capacity, 1) != capacity) {
        err = ERR_FAULT;
      }
    }

    if (err == ERR_OK) {
      err = socket_send_datagram(sock, &chain_buf);
    }

    /* deallocated the buffer */
    netbuf_free(&chain_buf);

    sock_set_errno(sock, err_to_errno(err));
    done_socket(sock);
    return (err == ERR_OK ? size : -1);
sendmsg_emsgsize:
    sock_set_errno(sock, EMSGSIZE);
    netbuf_free(&chain_buf);
    done_socket(sock);
    return -1;
  }
#else /* LWIP_UDP || LWIP_RAW */
  sock_set_errno(sock, err_to_errno(ERR_ARG));
  done_socket(sock);
  return -1;
#endif /* LWIP_UDP || LWIP_RAW */
}

ssize_t
lwip_sendto(int s, const void *data, size_t size, int flags,
            const struct sockaddr *to, socklen_t tolen)
{
  struct lwip_sock *sock;
  err_t err;
  u16_t short_size;
  struct netbuf buf;

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
#if LWIP_TCP
    done_socket(sock);
    return lwip_send(s, data, size, flags);
#else /* LWIP_TCP */
    LWIP_UNUSED_ARG(flags);
    sock_set_errno(sock, err_to_errno(ERR_ARG));
    done_socket(sock);
    return -1;
#endif /* LWIP_TCP */
  }

#if LWIP_UDP
  {
    int error = socket_datagram_prepare(sock, to != NULL, 0);

    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
  }
#endif /* LWIP_UDP */
  if (flags & MSG_OOB) {
    sock_set_errno(sock, EOPNOTSUPP);
    done_socket(sock);
    return -1;
  }

  if (size > LWIP_MIN(0xFFFF, SSIZE_MAX)) {
    /* cannot fit into one datagram (at least for us) */
    sock_set_errno(sock, EMSGSIZE);
    done_socket(sock);
    return -1;
  }
  short_size = (u16_t)size;
  buf.p = buf.ptr = NULL;
#if LWIP_CHECKSUM_ON_COPY
  buf.flags = 0;
#endif
  {
    int error = socket_datagram_destination(sock, to, tolen, &buf);

    if (error) {
      sock_set_errno(sock, error);
      done_socket(sock);
      return -1;
    }
  }
  if (uk_load_n(&sock->shutdown) & LWIP_SOCKET_SHUT_WR) {
    sock_set_errno(sock, EPIPE);
    done_socket(sock);
    return -1;
  }
  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_sendto(%d, data=%p, short_size=%"U16_F", flags=0x%x to=",
                              s, data, short_size, flags));
  ip_addr_debug_print_val(SOCKETS_DEBUG, buf.addr);
  LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%" U16_F "\n", netbuf_fromport(&buf)));

  /* make the buffer point to the data that should be sent */
  if (netbuf_alloc(&buf, short_size) == NULL) {
    err = ERR_MEM;
  } else if (uk_nofault_memcpy(buf.p->payload, data,
                              short_size, 0) != short_size) {
    err = ERR_FAULT;
  } else {
    err = ERR_OK;
  }

  if (err == ERR_OK) {
    err = socket_send_datagram(sock, &buf);
  }

  /* deallocated the buffer */
  netbuf_free(&buf);

  sock_set_errno(sock, err_to_errno(err));
  done_socket(sock);
  return (err == ERR_OK ? short_size : -1);
}

int
lwip_socket(int domain, int type, int protocol)
{
  struct netconn *conn;
  int i;

  LWIP_UNUSED_ARG(domain); /* @todo: check this */

  /* create a netconn */
  switch (type) {
    case SOCK_RAW:
      conn = netconn_new_with_proto_and_callback(DOMAIN_TO_NETCONN_TYPE(domain, NETCONN_RAW),
             (u8_t)protocol, DEFAULT_SOCKET_EVENTCB);
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_socket(%s, SOCK_RAW, %d) = ",
                                  domain == PF_INET ? "PF_INET" : "UNKNOWN", protocol));
      break;
    case SOCK_DGRAM:
      conn = netconn_new_with_callback(DOMAIN_TO_NETCONN_TYPE(domain,
                                       ((protocol == IPPROTO_UDPLITE) ? NETCONN_UDPLITE : NETCONN_UDP)),
                                       DEFAULT_SOCKET_EVENTCB);
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_socket(%s, SOCK_DGRAM, %d) = ",
                                  domain == PF_INET ? "PF_INET" : "UNKNOWN", protocol));
#if LWIP_NETBUF_RECVINFO
      if (conn) {
        /* netconn layer enables pktinfo by default, sockets default to off */
        conn->flags &= ~NETCONN_FLAG_PKTINFO;
      }
#endif /* LWIP_NETBUF_RECVINFO */
      break;
    case SOCK_STREAM:
      conn = netconn_new_with_callback(DOMAIN_TO_NETCONN_TYPE(domain, NETCONN_TCP), DEFAULT_SOCKET_EVENTCB);
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_socket(%s, SOCK_STREAM, %d) = ",
                                  domain == PF_INET ? "PF_INET" : "UNKNOWN", protocol));
      break;
    default:
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_socket(%d, %d/UNKNOWN, %d) = -1\n",
                                  domain, type, protocol));
      set_errno(EINVAL);
      return -1;
  }

  if (!conn) {
    LWIP_DEBUGF(SOCKETS_DEBUG, ("-1 / ENOBUFS (could not create netconn)\n"));
    set_errno(ENOBUFS);
    return -1;
  }

  i = alloc_socket(conn, 0);

  if (i == -1) {
    netconn_delete(conn);
    set_errno(ENFILE);
    return -1;
  }
  conn->socket = i;
  done_socket(&sockets[i - LWIP_SOCKET_OFFSET]);
  LWIP_DEBUGF(SOCKETS_DEBUG, ("%d\n", i));
  set_errno(0);
  return i;
}

ssize_t
lwip_write(int s, const void *data, size_t size)
{
  return lwip_send(s, data, size, 0);
}

ssize_t
lwip_writev(int s, const struct iovec *iov, int iovcnt)
{
  struct msghdr msg;

  msg.msg_name = NULL;
  msg.msg_namelen = 0;
  /* Hack: we have to cast via number to cast from 'const' pointer to non-const.
     Blame the opengroup standard for this inconsistency. */
  msg.msg_iov = LWIP_CONST_CAST(struct iovec *, iov);
  msg.msg_iovlen = iovcnt;
  msg.msg_control = NULL;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;
  return lwip_sendmsg(s, &msg, 0);
}

#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
/* Add select_cb to select_cb_list. */
static void
lwip_link_select_cb(struct lwip_select_cb *select_cb)
{
  LWIP_SOCKET_SELECT_DECL_PROTECT(lev);

  /* Protect the select_cb_list */
  LWIP_SOCKET_SELECT_PROTECT(lev);

  /* Put this select_cb on top of list */
  select_cb->next = select_cb_list;
  if (select_cb_list != NULL) {
    select_cb_list->prev = select_cb;
  }
  select_cb_list = select_cb;
#if !LWIP_TCPIP_CORE_LOCKING
  /* Increasing this counter tells select_check_waiters that the list has changed. */
  select_cb_ctr++;
#endif

  /* Now we can safely unprotect */
  LWIP_SOCKET_SELECT_UNPROTECT(lev);
}

/* Remove select_cb from select_cb_list. */
static void
lwip_unlink_select_cb(struct lwip_select_cb *select_cb)
{
  LWIP_SOCKET_SELECT_DECL_PROTECT(lev);

  /* Take us off the list */
  LWIP_SOCKET_SELECT_PROTECT(lev);
  if (select_cb->next != NULL) {
    select_cb->next->prev = select_cb->prev;
  }
  if (select_cb_list == select_cb) {
    LWIP_ASSERT("select_cb->prev == NULL", select_cb->prev == NULL);
    select_cb_list = select_cb->next;
  } else {
    LWIP_ASSERT("select_cb->prev != NULL", select_cb->prev != NULL);
    select_cb->prev->next = select_cb->next;
  }
#if !LWIP_TCPIP_CORE_LOCKING
  /* Increasing this counter tells select_check_waiters that the list has changed. */
  select_cb_ctr++;
#endif
  LWIP_SOCKET_SELECT_UNPROTECT(lev);
}
#endif /* LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL */

#if LWIP_SOCKET_SELECT
/**
 * Go through the readset and writeset lists and see which socket of the sockets
 * set in the sets has events. On return, readset, writeset and exceptset have
 * the sockets enabled that had events.
 *
 * @param maxfdp1 the highest socket index in the sets
 * @param readset_in    set of sockets to check for read events
 * @param writeset_in   set of sockets to check for write events
 * @param exceptset_in  set of sockets to check for error events
 * @param readset_out   set of sockets that had read events
 * @param writeset_out  set of sockets that had write events
 * @param exceptset_out set os sockets that had error events
 * @return number of sockets that had events (read/write/exception) (>= 0)
 */
static int
lwip_selscan(int maxfdp1, fd_set *readset_in, fd_set *writeset_in, fd_set *exceptset_in,
             fd_set *readset_out, fd_set *writeset_out, fd_set *exceptset_out)
{
  int i, nready = 0;
  fd_set lreadset, lwriteset, lexceptset;
  struct lwip_sock *sock;
  SYS_ARCH_DECL_PROTECT(lev);

  FD_ZERO(&lreadset);
  FD_ZERO(&lwriteset);
  FD_ZERO(&lexceptset);

  /* Go through each socket in each list to count number of sockets which
     currently match */
  for (i = LWIP_SOCKET_OFFSET; i < maxfdp1; i++) {
    /* if this FD is not in the set, continue */
    if (!(readset_in && FD_ISSET(i, readset_in)) &&
        !(writeset_in && FD_ISSET(i, writeset_in)) &&
        !(exceptset_in && FD_ISSET(i, exceptset_in))) {
      continue;
    }
    /* First get the socket's status (protected)... */
    SYS_ARCH_PROTECT(lev);
    sock = tryget_socket_unconn_locked(i);
    if (sock != NULL) {
      void *lastdata = sock->lastdata.pointer;
      s16_t rcvevent = sock->rcvevent;
      u16_t sendevent = sock->sendevent;
      u16_t errevent = sock->errevent;
      SYS_ARCH_UNPROTECT(lev);

      /* ... then examine it: */
      /* See if netconn of this socket is ready for read */
      if (readset_in && FD_ISSET(i, readset_in) && ((lastdata != NULL) || (rcvevent > 0))) {
        FD_SET(i, &lreadset);
        LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_selscan: fd=%d ready for reading\n", i));
        nready++;
      }
      /* See if netconn of this socket is ready for write */
      if (writeset_in && FD_ISSET(i, writeset_in) && (sendevent != 0)) {
        FD_SET(i, &lwriteset);
        LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_selscan: fd=%d ready for writing\n", i));
        nready++;
      }
      /* See if netconn of this socket had an error */
      if (exceptset_in && FD_ISSET(i, exceptset_in) && (errevent != 0)) {
        FD_SET(i, &lexceptset);
        LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_selscan: fd=%d ready for exception\n", i));
        nready++;
      }
      done_socket(sock);
    } else {
      SYS_ARCH_UNPROTECT(lev);
      /* no a valid open socket */
      return -1;
    }
  }
  /* copy local sets to the ones provided as arguments */
  *readset_out = lreadset;
  *writeset_out = lwriteset;
  *exceptset_out = lexceptset;

  LWIP_ASSERT("nready >= 0", nready >= 0);
  return nready;
}

#if LWIP_NETCONN_FULLDUPLEX
/* Mark all of the set sockets in one of the three fdsets passed to select as used.
 * All sockets are marked (and later unmarked), whether they are open or not.
 * This is OK as lwip_selscan aborts select when non-open sockets are found.
 */
static void
lwip_select_inc_sockets_used_set(int maxfdp, fd_set *fdset, fd_set *used_sockets)
{
  SYS_ARCH_DECL_PROTECT(lev);
  if (fdset) {
    int i;
    for (i = LWIP_SOCKET_OFFSET; i < maxfdp; i++) {
      /* if this FD is in the set, lock it (unless already done) */
      if (FD_ISSET(i, fdset) && !FD_ISSET(i, used_sockets)) {
        struct lwip_sock *sock;
        SYS_ARCH_PROTECT(lev);
        sock = tryget_socket_unconn_locked(i);
        if (sock != NULL) {
          /* leave the socket used until released by lwip_select_dec_sockets_used */
          FD_SET(i, used_sockets);
        }
        SYS_ARCH_UNPROTECT(lev);
      }
    }
  }
}

/* Mark all sockets passed to select as used to prevent them from being freed
 * from other threads while select is running.
 * Marked sockets are added to 'used_sockets' to mark them only once an be able
 * to unmark them correctly.
 */
static void
lwip_select_inc_sockets_used(int maxfdp, fd_set *fdset1, fd_set *fdset2, fd_set *fdset3, fd_set *used_sockets)
{
  FD_ZERO(used_sockets);
  lwip_select_inc_sockets_used_set(maxfdp, fdset1, used_sockets);
  lwip_select_inc_sockets_used_set(maxfdp, fdset2, used_sockets);
  lwip_select_inc_sockets_used_set(maxfdp, fdset3, used_sockets);
}

/* Let go all sockets that were marked as used when starting select */
static void
lwip_select_dec_sockets_used(int maxfdp, fd_set *used_sockets)
{
  int i;
  for (i = LWIP_SOCKET_OFFSET; i < maxfdp; i++) {
    /* if this FD is not in the set, continue */
    if (FD_ISSET(i, used_sockets)) {
      struct lwip_sock *sock = tryget_socket_unconn_nouse(i);
      LWIP_ASSERT("socket gone at the end of select", sock != NULL);
      if (sock != NULL) {
        done_socket(sock);
      }
    }
  }
}
#else /* LWIP_NETCONN_FULLDUPLEX */
#define lwip_select_inc_sockets_used(maxfdp1, readset, writeset, exceptset, used_sockets)
#define lwip_select_dec_sockets_used(maxfdp1, used_sockets)
#endif /* LWIP_NETCONN_FULLDUPLEX */

int
lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset, fd_set *exceptset,
            struct timeval *timeout)
{
  u32_t waitres = 0;
  int nready;
  fd_set lreadset, lwriteset, lexceptset;
  u32_t msectimeout;
  int i;
  int maxfdp2;
#if LWIP_NETCONN_SEM_PER_THREAD
  int waited = 0;
#endif
#if LWIP_NETCONN_FULLDUPLEX
  fd_set used_sockets;
#endif
  SYS_ARCH_DECL_PROTECT(lev);

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_select(%d, %p, %p, %p, tvsec=%"S32_F" tvusec=%"S32_F")\n",
                              maxfdp1, (void *)readset, (void *) writeset, (void *) exceptset,
                              timeout ? (s32_t)timeout->tv_sec : (s32_t) - 1,
                              timeout ? (s32_t)timeout->tv_usec : (s32_t) - 1));

  if ((maxfdp1 < 0) || (maxfdp1 > LWIP_SELECT_MAXNFDS)) {
    set_errno(EINVAL);
    return -1;
  }

  lwip_select_inc_sockets_used(maxfdp1, readset, writeset, exceptset, &used_sockets);

  /* Go through each socket in each list to count number of sockets which
     currently match */
  nready = lwip_selscan(maxfdp1, readset, writeset, exceptset, &lreadset, &lwriteset, &lexceptset);

  if (nready < 0) {
    /* one of the sockets in one of the fd_sets was invalid */
    set_errno(EBADF);
    lwip_select_dec_sockets_used(maxfdp1, &used_sockets);
    return -1;
  } else if (nready > 0) {
    /* one or more sockets are set, no need to wait */
    LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_select: nready=%d\n", nready));
  } else {
    /* If we don't have any current events, then suspend if we are supposed to */
    if (timeout && timeout->tv_sec == 0 && timeout->tv_usec == 0) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_select: no timeout, returning 0\n"));
      /* This is OK as the local fdsets are empty and nready is zero,
         or we would have returned earlier. */
    } else {
      /* None ready: add our semaphore to list:
         We don't actually need any dynamic memory. Our entry on the
         list is only valid while we are in this function, so it's ok
         to use local variables (unless we're running in MPU compatible
         mode). */
      API_SELECT_CB_VAR_DECLARE(select_cb);
      API_SELECT_CB_VAR_ALLOC(select_cb, set_errno(ENOMEM); lwip_select_dec_sockets_used(maxfdp1, &used_sockets); return -1);
      memset(&API_SELECT_CB_VAR_REF(select_cb), 0, sizeof(struct lwip_select_cb));

      API_SELECT_CB_VAR_REF(select_cb).readset = readset;
      API_SELECT_CB_VAR_REF(select_cb).writeset = writeset;
      API_SELECT_CB_VAR_REF(select_cb).exceptset = exceptset;
#if LWIP_NETCONN_SEM_PER_THREAD
      API_SELECT_CB_VAR_REF(select_cb).sem = LWIP_NETCONN_THREAD_SEM_GET();
#else /* LWIP_NETCONN_SEM_PER_THREAD */
      if (sys_sem_new(&API_SELECT_CB_VAR_REF(select_cb).sem, 0) != ERR_OK) {
        /* failed to create semaphore */
        set_errno(ENOMEM);
        lwip_select_dec_sockets_used(maxfdp1, &used_sockets);
        API_SELECT_CB_VAR_FREE(select_cb);
        return -1;
      }
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

      lwip_link_select_cb(&API_SELECT_CB_VAR_REF(select_cb));

      /* Increase select_waiting for each socket we are interested in */
      maxfdp2 = maxfdp1;
      for (i = LWIP_SOCKET_OFFSET; i < maxfdp1; i++) {
        if ((readset && FD_ISSET(i, readset)) ||
            (writeset && FD_ISSET(i, writeset)) ||
            (exceptset && FD_ISSET(i, exceptset))) {
          struct lwip_sock *sock;
          SYS_ARCH_PROTECT(lev);
          sock = tryget_socket_unconn_locked(i);
          if (sock != NULL) {
            sock->select_waiting++;
            if (sock->select_waiting == 0) {
              /* overflow - too many threads waiting */
              sock->select_waiting--;
              nready = -1;
              maxfdp2 = i;
              SYS_ARCH_UNPROTECT(lev);
              done_socket(sock);
              set_errno(EBUSY);
              break;
            }
            SYS_ARCH_UNPROTECT(lev);
            done_socket(sock);
          } else {
            /* Not a valid socket */
            nready = -1;
            maxfdp2 = i;
            SYS_ARCH_UNPROTECT(lev);
            set_errno(EBADF);
            break;
          }
        }
      }

      if (nready >= 0) {
        /* Call lwip_selscan again: there could have been events between
           the last scan (without us on the list) and putting us on the list! */
        nready = lwip_selscan(maxfdp1, readset, writeset, exceptset, &lreadset, &lwriteset, &lexceptset);
        if (nready < 0) {
          set_errno(EBADF);
        } else if (!nready) {
          /* Still none ready, just wait to be woken */
          if (timeout == 0) {
            /* Wait forever */
            msectimeout = 0;
          } else {
            long msecs_long = ((timeout->tv_sec * 1000) + ((timeout->tv_usec + 500) / 1000));
            if (msecs_long <= 0) {
              /* Wait 1ms at least (0 means wait forever) */
              msectimeout = 1;
            } else {
              msectimeout = (u32_t)msecs_long;
            }
          }

          waitres = sys_arch_sem_wait(SELECT_SEM_PTR(API_SELECT_CB_VAR_REF(select_cb).sem), msectimeout);
#if LWIP_NETCONN_SEM_PER_THREAD
          waited = 1;
#endif
        }
      }

      /* Decrease select_waiting for each socket we are interested in */
      for (i = LWIP_SOCKET_OFFSET; i < maxfdp2; i++) {
        if ((readset && FD_ISSET(i, readset)) ||
            (writeset && FD_ISSET(i, writeset)) ||
            (exceptset && FD_ISSET(i, exceptset))) {
          struct lwip_sock *sock;
          SYS_ARCH_PROTECT(lev);
          sock = tryget_socket_unconn_nouse(i);
          LWIP_ASSERT("socket gone at the end of select", sock != NULL);
          if (sock != NULL) {
            /* for now, handle select_waiting==0... */
            LWIP_ASSERT("sock->select_waiting > 0", sock->select_waiting > 0);
            if (sock->select_waiting > 0) {
              sock->select_waiting--;
            }
            SYS_ARCH_UNPROTECT(lev);
          } else {
            SYS_ARCH_UNPROTECT(lev);
            /* Not a valid socket */
            nready = -1;
            set_errno(EBADF);
          }
        }
      }

      lwip_unlink_select_cb(&API_SELECT_CB_VAR_REF(select_cb));

#if LWIP_NETCONN_SEM_PER_THREAD
      if (API_SELECT_CB_VAR_REF(select_cb).sem_signalled && (!waited || (waitres == SYS_ARCH_TIMEOUT))) {
        /* don't leave the thread-local semaphore signalled */
        sys_arch_sem_wait(API_SELECT_CB_VAR_REF(select_cb).sem, 1);
      }
#else /* LWIP_NETCONN_SEM_PER_THREAD */
      sys_sem_free(&API_SELECT_CB_VAR_REF(select_cb).sem);
#endif /* LWIP_NETCONN_SEM_PER_THREAD */
      API_SELECT_CB_VAR_FREE(select_cb);

      if (nready < 0) {
        /* This happens when a socket got closed while waiting */
        lwip_select_dec_sockets_used(maxfdp1, &used_sockets);
        return -1;
      }

      if (waitres == SYS_ARCH_TIMEOUT) {
        /* Timeout */
        LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_select: timeout expired\n"));
        /* This is OK as the local fdsets are empty and nready is zero,
           or we would have returned earlier. */
      } else {
        /* See what's set now after waiting */
        nready = lwip_selscan(maxfdp1, readset, writeset, exceptset, &lreadset, &lwriteset, &lexceptset);
        LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_select: nready=%d\n", nready));
        if (nready < 0) {
          set_errno(EBADF);
          lwip_select_dec_sockets_used(maxfdp1, &used_sockets);
          return -1;
        }
      }
    }
  }

  lwip_select_dec_sockets_used(maxfdp1, &used_sockets);
  set_errno(0);
  if (readset) {
    *readset = lreadset;
  }
  if (writeset) {
    *writeset = lwriteset;
  }
  if (exceptset) {
    *exceptset = lexceptset;
  }
  return nready;
}
#endif /* LWIP_SOCKET_SELECT */

#if LWIP_SOCKET_POLL
/** Options for the lwip_pollscan function. */
enum lwip_pollscan_opts
{
  /** Clear revents in each struct pollfd. */
  LWIP_POLLSCAN_CLEAR = 1,

  /** Increment select_waiting in each struct lwip_sock. */
  LWIP_POLLSCAN_INC_WAIT = 2,

  /** Decrement select_waiting in each struct lwip_sock. */
  LWIP_POLLSCAN_DEC_WAIT = 4
};

/**
 * Update revents in each struct pollfd.
 * Optionally update select_waiting in struct lwip_sock.
 *
 * @param fds          array of structures to update
 * @param nfds         number of structures in fds
 * @param opts         what to update and how
 * @return number of structures that have revents != 0
 */
static int
lwip_pollscan(struct pollfd *fds, nfds_t nfds, enum lwip_pollscan_opts opts)
{
  int nready = 0;
  nfds_t fdi;
  struct lwip_sock *sock;
  SYS_ARCH_DECL_PROTECT(lev);

  /* Go through each struct pollfd in the array. */
  for (fdi = 0; fdi < nfds; fdi++) {
    if ((opts & LWIP_POLLSCAN_CLEAR) != 0) {
      fds[fdi].revents = 0;
    }

    /* Negative fd means the caller wants us to ignore this struct.
       POLLNVAL means we already detected that the fd is invalid;
       if another thread has since opened a new socket with that fd,
       we must not use that socket. */
    if (fds[fdi].fd >= 0 && (fds[fdi].revents & POLLNVAL) == 0) {
      /* First get the socket's status (protected)... */
      SYS_ARCH_PROTECT(lev);
      sock = tryget_socket_unconn_locked(fds[fdi].fd);
      if (sock != NULL) {
        void *lastdata = sock->lastdata.pointer;
        s16_t rcvevent = sock->rcvevent;
        u16_t sendevent = sock->sendevent;
        u16_t errevent = sock->errevent;

        if ((opts & LWIP_POLLSCAN_INC_WAIT) != 0) {
          sock->select_waiting++;
          if (sock->select_waiting == 0) {
            /* overflow - too many threads waiting */
            sock->select_waiting--;
            nready = -1;
            SYS_ARCH_UNPROTECT(lev);
            done_socket(sock);
            break;
          }
        } else if ((opts & LWIP_POLLSCAN_DEC_WAIT) != 0) {
          /* for now, handle select_waiting==0... */
          LWIP_ASSERT("sock->select_waiting > 0", sock->select_waiting > 0);
          if (sock->select_waiting > 0) {
            sock->select_waiting--;
          }
        }
        SYS_ARCH_UNPROTECT(lev);
        done_socket(sock);

        /* ... then examine it: */
        /* See if netconn of this socket is ready for read */
        if ((fds[fdi].events & POLLIN) != 0 && ((lastdata != NULL) || (rcvevent > 0))) {
          fds[fdi].revents |= POLLIN;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_pollscan: fd=%d ready for reading\n", fds[fdi].fd));
        }
        /* See if netconn of this socket is ready for write */
        if ((fds[fdi].events & POLLOUT) != 0 && (sendevent != 0)) {
          fds[fdi].revents |= POLLOUT;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_pollscan: fd=%d ready for writing\n", fds[fdi].fd));
        }
        /* See if netconn of this socket had an error */
        if (errevent != 0) {
          /* POLLERR is output only. */
          fds[fdi].revents |= POLLERR;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_pollscan: fd=%d ready for exception\n", fds[fdi].fd));
        }
      } else {
        /* Not a valid socket */
        SYS_ARCH_UNPROTECT(lev);
        /* POLLNVAL is output only. */
        fds[fdi].revents |= POLLNVAL;
        return -1;
      }
    }

    /* Will return the number of structures that have events,
       not the number of events. */
    if (fds[fdi].revents != 0) {
      nready++;
    }
  }

  LWIP_ASSERT("nready >= 0", nready >= 0);
  return nready;
}

#if LWIP_NETCONN_FULLDUPLEX
/* Mark all sockets as used.
 *
 * All sockets are marked (and later unmarked), whether they are open or not.
 * This is OK as lwip_pollscan aborts select when non-open sockets are found.
 */
static void
lwip_poll_inc_sockets_used(struct pollfd *fds, nfds_t nfds)
{
  nfds_t fdi;

  if(fds) {
    /* Go through each struct pollfd in the array. */
    for (fdi = 0; fdi < nfds; fdi++) {
      /* Increase the reference counter */
      tryget_socket_unconn(fds[fdi].fd);
    }
  }
}

/* Let go all sockets that were marked as used when starting poll */
static void
lwip_poll_dec_sockets_used(struct pollfd *fds, nfds_t nfds)
{
  nfds_t fdi;

  if(fds) {
    /* Go through each struct pollfd in the array. */
    for (fdi = 0; fdi < nfds; fdi++) {
      struct lwip_sock *sock = tryget_socket_unconn_nouse(fds[fdi].fd);
      if (sock != NULL) {
        done_socket(sock);
      }
    }
  }
}
#else /* LWIP_NETCONN_FULLDUPLEX */
#define lwip_poll_inc_sockets_used(fds, nfds)
#define lwip_poll_dec_sockets_used(fds, nfds)
#endif /* LWIP_NETCONN_FULLDUPLEX */

int
lwip_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
  u32_t waitres = 0;
  int nready;
  u32_t msectimeout;
#if LWIP_NETCONN_SEM_PER_THREAD
  int waited = 0;
#endif

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_poll(%p, %d, %d)\n",
                  (void*)fds, (int)nfds, timeout));
  LWIP_ERROR("lwip_poll: invalid fds", ((fds != NULL && nfds > 0) || (fds == NULL && nfds == 0)),
             set_errno(EINVAL); return -1;);

  lwip_poll_inc_sockets_used(fds, nfds);

  /* Go through each struct pollfd to count number of structures
     which currently match */
  nready = lwip_pollscan(fds, nfds, LWIP_POLLSCAN_CLEAR);

  if (nready < 0) {
    lwip_poll_dec_sockets_used(fds, nfds);
    return -1;
  }

  /* If we don't have any current events, then suspend if we are supposed to */
  if (!nready) {
    API_SELECT_CB_VAR_DECLARE(select_cb);

    if (timeout == 0) {
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_poll: no timeout, returning 0\n"));
      goto return_success;
    }
    API_SELECT_CB_VAR_ALLOC(select_cb, set_errno(EAGAIN); lwip_poll_dec_sockets_used(fds, nfds); return -1);
    memset(&API_SELECT_CB_VAR_REF(select_cb), 0, sizeof(struct lwip_select_cb));

    /* None ready: add our semaphore to list:
       We don't actually need any dynamic memory. Our entry on the
       list is only valid while we are in this function, so it's ok
       to use local variables. */

    API_SELECT_CB_VAR_REF(select_cb).poll_fds = fds;
    API_SELECT_CB_VAR_REF(select_cb).poll_nfds = nfds;
#if LWIP_NETCONN_SEM_PER_THREAD
    API_SELECT_CB_VAR_REF(select_cb).sem = LWIP_NETCONN_THREAD_SEM_GET();
#else /* LWIP_NETCONN_SEM_PER_THREAD */
    if (sys_sem_new(&API_SELECT_CB_VAR_REF(select_cb).sem, 0) != ERR_OK) {
      /* failed to create semaphore */
      set_errno(EAGAIN);
      lwip_poll_dec_sockets_used(fds, nfds);
      API_SELECT_CB_VAR_FREE(select_cb);
      return -1;
    }
#endif /* LWIP_NETCONN_SEM_PER_THREAD */

    lwip_link_select_cb(&API_SELECT_CB_VAR_REF(select_cb));

    /* Increase select_waiting for each socket we are interested in.
       Also, check for events again: there could have been events between
       the last scan (without us on the list) and putting us on the list! */
    nready = lwip_pollscan(fds, nfds, LWIP_POLLSCAN_INC_WAIT);

    if (!nready) {
      /* Still none ready, just wait to be woken */
      if (timeout < 0) {
        /* Wait forever */
        msectimeout = 0;
      } else {
        /* timeout == 0 would have been handled earlier. */
        LWIP_ASSERT("timeout > 0", timeout > 0);
        msectimeout = timeout;
      }
      waitres = sys_arch_sem_wait(SELECT_SEM_PTR(API_SELECT_CB_VAR_REF(select_cb).sem), msectimeout);
#if LWIP_NETCONN_SEM_PER_THREAD
      waited = 1;
#endif
    }

    /* Decrease select_waiting for each socket we are interested in,
       and check which events occurred while we waited. */
    nready = lwip_pollscan(fds, nfds, LWIP_POLLSCAN_DEC_WAIT);

    lwip_unlink_select_cb(&API_SELECT_CB_VAR_REF(select_cb));

#if LWIP_NETCONN_SEM_PER_THREAD
    if (select_cb.sem_signalled && (!waited || (waitres == SYS_ARCH_TIMEOUT))) {
      /* don't leave the thread-local semaphore signalled */
      sys_arch_sem_wait(API_SELECT_CB_VAR_REF(select_cb).sem, 1);
    }
#else /* LWIP_NETCONN_SEM_PER_THREAD */
    sys_sem_free(&API_SELECT_CB_VAR_REF(select_cb).sem);
#endif /* LWIP_NETCONN_SEM_PER_THREAD */
    API_SELECT_CB_VAR_FREE(select_cb);

    if (nready < 0) {
      /* This happens when a socket got closed while waiting */
      lwip_poll_dec_sockets_used(fds, nfds);
      return -1;
    }

    if (waitres == SYS_ARCH_TIMEOUT) {
      /* Timeout */
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_poll: timeout expired\n"));
      goto return_success;
    }
  }

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_poll: nready=%d\n", nready));
return_success:
  lwip_poll_dec_sockets_used(fds, nfds);
  set_errno(0);
  return nready;
}

/**
 * Check whether event_callback should wake up a thread waiting in
 * lwip_poll.
 */
static int
lwip_poll_should_wake(const struct lwip_select_cb *scb, int fd, int has_recvevent, int has_sendevent, int has_errevent)
{
  nfds_t fdi;
  for (fdi = 0; fdi < scb->poll_nfds; fdi++) {
    const struct pollfd *pollfd = &scb->poll_fds[fdi];
    if (pollfd->fd == fd) {
      /* Do not update pollfd->revents right here;
         that would be a data race because lwip_pollscan
         accesses revents without protecting. */
      if (has_recvevent && (pollfd->events & POLLIN) != 0) {
        return 1;
      }
      if (has_sendevent && (pollfd->events & POLLOUT) != 0) {
        return 1;
      }
      if (has_errevent) {
        /* POLLERR is output only. */
        return 1;
      }
    }
  }
  return 0;
}
#endif /* LWIP_SOCKET_POLL */

#if LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL
/**
 * Callback registered in the netconn layer for each socket-netconn.
 * Processes recvevent (data available) and wakes up tasks waiting for select.
 *
 * @note for LWIP_TCPIP_CORE_LOCKING any caller of this function
 * must have the core lock held when signaling the following events
 * as they might cause select_list_cb to be checked:
 *   NETCONN_EVT_RCVPLUS
 *   NETCONN_EVT_SENDPLUS
 *   NETCONN_EVT_ERROR
 * This requirement will be asserted in select_check_waiters()
 */
static void
event_callback(struct netconn *conn, enum netconn_evt evt, u16_t len)
{
  int s, check_waiters;
  struct lwip_sock *sock;
  SYS_ARCH_DECL_PROTECT(lev);

  LWIP_UNUSED_ARG(len);

  /* Get socket */
  if (conn) {
    s = conn->socket;
    if (s < 0) {
      /* Data comes in right away after an accept, even though
       * the server task might not have created a new socket yet.
       * Just count down (or up) if that's the case and we
       * will use the data later. Note that only receive events
       * can happen before the new socket is set up. */
      SYS_ARCH_PROTECT(lev);
      if (conn->socket < 0) {
        if (evt == NETCONN_EVT_RCVPLUS) {
          /* conn->socket is -1 on initialization
             lwip_accept adjusts sock->recvevent if conn->socket < -1 */
          conn->socket--;
        }
        SYS_ARCH_UNPROTECT(lev);
        return;
      }
      s = conn->socket;
      SYS_ARCH_UNPROTECT(lev);
    }

    sock = get_socket(s);
    if (!sock) {
      return;
    }
  } else {
    return;
  }

  check_waiters = 1;
  SYS_ARCH_PROTECT(lev);
  /* Set event as required */
  switch (evt) {
    case NETCONN_EVT_RCVPLUS:
      sock->rcvevent++;
      if (sock->rcvevent > 1) {
        check_waiters = 0;
      }
      break;
    case NETCONN_EVT_RCVMINUS:
      sock->rcvevent--;
      check_waiters = 0;
      break;
    case NETCONN_EVT_SENDPLUS:
      if (sock->sendevent) {
        check_waiters = 0;
      }
      sock->sendevent = 1;
      break;
    case NETCONN_EVT_SENDMINUS:
      sock->sendevent = 0;
      check_waiters = 0;
      break;
    case NETCONN_EVT_ERROR:
      sock->errevent = 1;
      break;
    default:
      LWIP_ASSERT("unknown event", 0);
      break;
  }

  lwip_posix_socket_event_callback(sock, evt, len);

  if (sock->select_waiting && check_waiters) {
    /* Save which events are active */
    int has_recvevent, has_sendevent, has_errevent;
    has_recvevent = sock->rcvevent > 0;
    has_sendevent = sock->sendevent != 0;
    has_errevent = sock->errevent != 0;
    SYS_ARCH_UNPROTECT(lev);
    /* Check any select calls waiting on this socket */
    select_check_waiters(s, has_recvevent, has_sendevent, has_errevent);
  } else {
    SYS_ARCH_UNPROTECT(lev);
  }
  done_socket(sock);
}

/**
 * Check if any select waiters are waiting on this socket and its events
 *
 * @note on synchronization of select_cb_list:
 * LWIP_TCPIP_CORE_LOCKING: the select_cb_list must only be accessed while holding
 * the core lock. We do a single pass through the list and signal any waiters.
 * Core lock should already be held when calling here!!!!

 * !LWIP_TCPIP_CORE_LOCKING: we use SYS_ARCH_PROTECT but unlock on each iteration
 * of the loop, thus creating a possibility where a thread could modify the
 * select_cb_list during our UNPROTECT/PROTECT. We use a generational counter to
 * detect this change and restart the list walk. The list is expected to be small
 */
static void select_check_waiters(int s, int has_recvevent, int has_sendevent, int has_errevent)
{
  struct lwip_select_cb *scb;
#if !LWIP_TCPIP_CORE_LOCKING
  int last_select_cb_ctr;
  SYS_ARCH_DECL_PROTECT(lev);
#endif /* !LWIP_TCPIP_CORE_LOCKING */

  LWIP_ASSERT_CORE_LOCKED();

#if !LWIP_TCPIP_CORE_LOCKING
  SYS_ARCH_PROTECT(lev);
again:
  /* remember the state of select_cb_list to detect changes */
  last_select_cb_ctr = select_cb_ctr;
#endif /* !LWIP_TCPIP_CORE_LOCKING */
  for (scb = select_cb_list; scb != NULL; scb = scb->next) {
    if (scb->sem_signalled == 0) {
      /* semaphore not signalled yet */
      int do_signal = 0;
#if LWIP_SOCKET_POLL
      if (scb->poll_fds != NULL) {
        do_signal = lwip_poll_should_wake(scb, s, has_recvevent, has_sendevent, has_errevent);
      }
#endif /* LWIP_SOCKET_POLL */
#if LWIP_SOCKET_SELECT && LWIP_SOCKET_POLL
      else
#endif /* LWIP_SOCKET_SELECT && LWIP_SOCKET_POLL */
#if LWIP_SOCKET_SELECT
      {
        /* Test this select call for our socket */
        if (has_recvevent) {
          if (scb->readset && FD_ISSET(s, scb->readset)) {
            do_signal = 1;
          }
        }
        if (has_sendevent) {
          if (!do_signal && scb->writeset && FD_ISSET(s, scb->writeset)) {
            do_signal = 1;
          }
        }
        if (has_errevent) {
          if (!do_signal && scb->exceptset && FD_ISSET(s, scb->exceptset)) {
            do_signal = 1;
          }
        }
      }
#endif /* LWIP_SOCKET_SELECT */
      if (do_signal) {
        scb->sem_signalled = 1;
        /* For !LWIP_TCPIP_CORE_LOCKING, we don't call SYS_ARCH_UNPROTECT() before signaling
           the semaphore, as this might lead to the select thread taking itself off the list,
           invalidating the semaphore. */
        sys_sem_signal(SELECT_SEM_PTR(scb->sem));
      }
    }
#if LWIP_TCPIP_CORE_LOCKING
  }
#else
    /* unlock interrupts with each step */
    SYS_ARCH_UNPROTECT(lev);
    /* this makes sure interrupt protection time is short */
    SYS_ARCH_PROTECT(lev);
    if (last_select_cb_ctr != select_cb_ctr) {
      /* someone has changed select_cb_list, restart at the beginning */
      goto again;
    }
    /* remember the state of select_cb_list to detect changes */
    last_select_cb_ctr = select_cb_ctr;
  }
  SYS_ARCH_UNPROTECT(lev);
#endif
}
#endif /* LWIP_SOCKET_SELECT || LWIP_SOCKET_POLL */

#if LWIP_UDP
struct socket_udp_shutdown {
  struct tcpip_api_call_data call;
  struct lwip_sock *socket;
  unsigned int mask;
};

static err_t socket_udp_shutdown(struct tcpip_api_call_data *call)
{
  struct socket_udp_shutdown *request =
    __containerof(call, struct socket_udp_shutdown, call);
  struct udp_pcb *pcb = request->socket->conn->pcb.udp;

  uk_or(&request->socket->shutdown, request->mask);
  return pcb && (pcb->flags & UDP_FLAGS_CONNECTED) ? ERR_OK : ERR_CONN;
}
#endif

int
lwip_shutdown(int s, int how)
{
  struct lwip_sock *sock = get_socket(s);
  err_t err;
  u8_t shut_rx, shut_tx;

  if (!sock) {
    return -1;
  }
  if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR) {
    sock_set_errno(sock, EINVAL);
    done_socket(sock);
    return -1;
  }
  shut_rx = how != SHUT_WR;
  shut_tx = how != SHUT_RD;
  if (!sock->conn) {
    sock_set_errno(sock, ENOTCONN);
    done_socket(sock);
    return -1;
  }
#if LWIP_UDP
  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_UDP) {
    struct socket_udp_shutdown request = {
      .socket = sock,
      .mask = (shut_rx ? LWIP_SOCKET_SHUT_RD : 0) |
              (shut_tx ? LWIP_SOCKET_SHUT_WR : 0),
    };
    err = tcpip_api_call(socket_udp_shutdown, &request.call);
  } else
#endif
  if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
    err = netconn_shutdown(sock->conn, shut_rx, shut_tx);
  } else {
    sock_set_errno(sock, EOPNOTSUPP);
    done_socket(sock);
    return -1;
  }
  sock_set_errno(sock, err_to_errno(err));
  done_socket(sock);
  return err == ERR_OK ? 0 : -1;
}

static int
lwip_getaddrname(int s, struct sockaddr *name, socklen_t *namelen, u8_t local)
{
  struct lwip_sock *sock;
  union sockaddr_aligned saddr;
  ip_addr_t naddr;
  u16_t port;
  err_t err;

  sock = get_socket(s);
  if (!sock) {
    return -1;
  }

  /* get the IP address and port */
  err = netconn_getaddr(sock->conn, &naddr, &port, local);
  if (err != ERR_OK) {
    sock_set_errno(sock, err_to_errno(err));
    done_socket(sock);
    return -1;
  }

#if LWIP_IPV4 && LWIP_IPV6
  /* Dual-stack: Map IPv4 addresses to IPv4 mapped IPv6 */
  if (NETCONNTYPE_ISIPV6(netconn_type(sock->conn)) &&
      IP_IS_V4_VAL(naddr)) {
    ip4_2_ipv4_mapped_ipv6(ip_2_ip6(&naddr), ip_2_ip4(&naddr));
    IP_SET_TYPE_VAL(naddr, IPADDR_TYPE_V6);
  }
#endif /* LWIP_IPV4 && LWIP_IPV6 */

  IPADDR_PORT_TO_SOCKADDR(&saddr, &naddr, port);

  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getaddrname(%d, addr=", s));
  ip_addr_debug_print_val(SOCKETS_DEBUG, naddr);
  LWIP_DEBUGF(SOCKETS_DEBUG, (" port=%"U16_F")\n", port));

  if (*namelen > sizeof(*name)) {
    *namelen = sizeof(*name);
  }
  MEMCPY(name, &saddr, *namelen);

  sock_set_errno(sock, 0);
  done_socket(sock);
  return 0;
}

int
lwip_getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
  return lwip_getaddrname(s, name, namelen, 0);
}

int
lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
  return lwip_getaddrname(s, name, namelen, 1);
}

int
lwip_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen)
{
  int err;
  struct lwip_sock *sock = get_socket(s);
#if !LWIP_TCPIP_CORE_LOCKING
  err_t cberr;
  LWIP_SETGETSOCKOPT_DATA_VAR_DECLARE(data);
#endif /* !LWIP_TCPIP_CORE_LOCKING */

  if (!sock) {
    return -1;
  }

  if (NULL == optlen) {
    sock_set_errno(sock, EFAULT);
    done_socket(sock);
    return -1;
  }

#if LWIP_TCPIP_CORE_LOCKING
  /* core-locking can just call the -impl function */
  LOCK_TCPIP_CORE();
  err = lwip_getsockopt_impl(s, level, optname, optval, optlen);
  UNLOCK_TCPIP_CORE();

#else /* LWIP_TCPIP_CORE_LOCKING */

#if LWIP_MPU_COMPATIBLE
  /* MPU_COMPATIBLE copies the optval data, so check for max size here */
  if (*optlen > LWIP_SETGETSOCKOPT_MAXOPTLEN) {
    sock_set_errno(sock, ENOBUFS);
    done_socket(sock);
    return -1;
  }
#endif /* LWIP_MPU_COMPATIBLE */

  LWIP_SETGETSOCKOPT_DATA_VAR_ALLOC(data, sock);
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).s = s;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).level = level;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optname = optname;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optlen = *optlen;
#if !LWIP_MPU_COMPATIBLE
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optval.p = optval;
#endif /* !LWIP_MPU_COMPATIBLE */
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).err = 0;
#if LWIP_NETCONN_SEM_PER_THREAD
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem = LWIP_NETCONN_THREAD_SEM_GET();
#else
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem = &sock->conn->op_completed;
#endif
  cberr = tcpip_callback(lwip_getsockopt_callback, &LWIP_SETGETSOCKOPT_DATA_VAR_REF(data));
  if (cberr != ERR_OK) {
    LWIP_SETGETSOCKOPT_DATA_VAR_FREE(data);
    sock_set_errno(sock, err_to_errno(cberr));
    done_socket(sock);
    return -1;
  }
  sys_arch_sem_wait((sys_sem_t *)(LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem), 0);

  /* write back optlen and optval */
  *optlen = LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optlen;
#if LWIP_MPU_COMPATIBLE
  MEMCPY(optval, LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optval,
         LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optlen);
#endif /* LWIP_MPU_COMPATIBLE */

  /* maybe lwip_getsockopt_internal has changed err */
  err = LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).err;
  LWIP_SETGETSOCKOPT_DATA_VAR_FREE(data);
#endif /* LWIP_TCPIP_CORE_LOCKING */

  sock_set_errno(sock, err);
  done_socket(sock);
  return err ? -1 : 0;
}

#if !LWIP_TCPIP_CORE_LOCKING
/** lwip_getsockopt_callback: only used without CORE_LOCKING
 * to get into the tcpip_thread
 */
static void
lwip_getsockopt_callback(void *arg)
{
  struct lwip_setgetsockopt_data *data;
  LWIP_ASSERT("arg != NULL", arg != NULL);
  data = (struct lwip_setgetsockopt_data *)arg;

  data->err = lwip_getsockopt_impl(data->s, data->level, data->optname,
#if LWIP_MPU_COMPATIBLE
                                   data->optval,
#else /* LWIP_MPU_COMPATIBLE */
                                   data->optval.p,
#endif /* LWIP_MPU_COMPATIBLE */
                                   &data->optlen);

  sys_sem_signal((sys_sem_t *)(data->completed_sem));
}
#endif  /* LWIP_TCPIP_CORE_LOCKING */

static int
lwip_sockopt_to_ipopt(int optname)
{
  /* Map SO_* values to our internal SOF_* values
   * We should not rely on #defines in socket.h
   * being in sync with ip.h.
   */
  switch (optname) {
  case SO_BROADCAST:
    return SOF_BROADCAST;
  case SO_KEEPALIVE:
    return SOF_KEEPALIVE;
  case SO_REUSEADDR:
    return SOF_REUSEADDR;
  default:
    LWIP_ASSERT("Unknown socket option", 0);
    return 0;
  }
}

/** lwip_getsockopt_impl: the actual implementation of getsockopt:
 * same argument as lwip_getsockopt, either called directly or through callback
 */
static int
lwip_getsockopt_impl(int s, int level, int optname, void *optval, socklen_t *optlen)
{
  int err = 0;
  const int option_input = 0;
  union lwip_sockopt_value option_value = {0};
  size_t option_length = 0;
  void *user_value = optval;
  struct lwip_sock *sock = tryget_socket(s);
  if (!sock) {
    return EBADF;
  }

#ifdef LWIP_HOOK_SOCKETS_GETSOCKOPT
  if (LWIP_HOOK_SOCKETS_GETSOCKOPT(s, sock, level, optname, optval, optlen, &err)) {
    return err;
  }
#endif

  optval = &option_value;

  switch (level) {

    /* Level: SOL_SOCKET */
    case SOL_SOCKET:
      switch (optname) {

        case SO_TIMESTAMP:
        case SO_TIMESTAMPNS:
        case SO_TIMESTAMP_NEW:
        case SO_TIMESTAMPNS_NEW:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, int);
          {
            unsigned int mode = uk_load_n(&sock->timestamp_mode);
            int nanoseconds = optname == SO_TIMESTAMPNS || optname == SO_TIMESTAMPNS_NEW;

            *(int *)optval = mode &&
              (nanoseconds == (mode == SO_TIMESTAMPNS || mode == SO_TIMESTAMPNS_NEW));
          }
          break;


#if LWIP_TCP
        case SO_ACCEPTCONN:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, int);
          if (NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_TCP) {
            done_socket(sock);
            return ENOPROTOOPT;
          }
          if ((sock->conn->pcb.tcp != NULL) && (sock->conn->pcb.tcp->state == LISTEN)) {
            *(int *)optval = 1;
          } else {
            *(int *)optval = 0;
          }
          break;
#endif /* LWIP_TCP */

        /* The option flags */
        case SO_BROADCAST:
        case SO_KEEPALIVE:
#if SO_REUSE
        case SO_REUSEADDR:
#endif /* SO_REUSE */
          if ((optname == SO_BROADCAST) &&
              (NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_UDP)) {
            done_socket(sock);
            return ENOPROTOOPT;
          }

          optname = lwip_sockopt_to_ipopt(optname);

          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, int);
          *(int *)optval = ip_get_option(sock->conn->pcb.ip, optname);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, SOL_SOCKET, optname=0x%x, ..) = %s\n",
                                      s, optname, (*(int *)optval ? "on" : "off")));
          break;

        case SO_TYPE:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, int);
          switch (NETCONNTYPE_GROUP(netconn_type(sock->conn))) {
            case NETCONN_RAW:
              *(int *)optval = SOCK_RAW;
              break;
            case NETCONN_TCP:
              *(int *)optval = SOCK_STREAM;
              break;
            case NETCONN_UDP:
              *(int *)optval = SOCK_DGRAM;
              break;
            default: /* unrecognized socket type */
              *(int *)optval = netconn_type(sock->conn);
              LWIP_DEBUGF(SOCKETS_DEBUG,
                          ("lwip_getsockopt(%d, SOL_SOCKET, SO_TYPE): unrecognized socket type %d\n",
                           s, *(int *)optval));
          }  /* switch (netconn_type(sock->conn)) */
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, SOL_SOCKET, SO_TYPE) = %d\n",
                                      s, *(int *)optval));
          break;

        case SO_ERROR:
          LWIP_SOCKOPT_CHECK_OPTLEN(sock, *optlen, int);
          *(int *)optval = err_to_errno(netconn_err(sock->conn));
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, SOL_SOCKET, SO_ERROR) = %d\n",
                                      s, *(int *)optval));
          break;

#if LWIP_SO_SNDTIMEO
        case SO_SNDTIMEO:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, LWIP_SO_SNDRCVTIMEO_OPTTYPE);
          LWIP_SO_SNDRCVTIMEO_SET(optval, netconn_get_sendtimeout(sock->conn));
          break;
#endif /* LWIP_SO_SNDTIMEO */
#if LWIP_SO_RCVTIMEO
        case SO_RCVTIMEO:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, LWIP_SO_SNDRCVTIMEO_OPTTYPE);
          LWIP_SO_SNDRCVTIMEO_SET(optval, netconn_get_recvtimeout(sock->conn));
          break;
#endif /* LWIP_SO_RCVTIMEO */
#if LWIP_TCP
        case SO_SNDBUF:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, int);
          if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
            done_socket(sock);
            return ENOPROTOOPT;
          }
          *(int *)optval = (int)sock->conn->send_bufsize;
          break;
#endif /* LWIP_TCP */
#if LWIP_SO_RCVBUF
        case SO_RCVBUF:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, int);
          *(int *)optval = netconn_get_recvbufsize(sock->conn);
          break;
#endif /* LWIP_SO_RCVBUF */
#if LWIP_SO_LINGER
        case SO_LINGER: {
          s16_t conn_linger;
          struct linger *linger = (struct linger *)optval;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, struct linger);
          conn_linger = sock->conn->linger;
          if (conn_linger >= 0) {
            linger->l_onoff = 1;
            linger->l_linger = (int)conn_linger;
          } else {
            linger->l_onoff = 0;
            linger->l_linger = 0;
          }
        }
        break;
#endif /* LWIP_SO_LINGER */
#if LWIP_UDP
        case SO_NO_CHECK:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, *optlen, int, NETCONN_UDP);
#if LWIP_UDPLITE
          if (udp_is_flag_set(sock->conn->pcb.udp, UDP_FLAGS_UDPLITE)) {
            /* this flag is only available for UDP, not for UDP lite */
            done_socket(sock);
            return EAFNOSUPPORT;
          }
#endif /* LWIP_UDPLITE */
          *(int *)optval = udp_is_flag_set(sock->conn->pcb.udp, UDP_FLAGS_NOCHKSUM) ? 1 : 0;
          break;
#endif /* LWIP_UDP*/
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, SOL_SOCKET, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;

    /* Level: IPPROTO_IP */
    case IPPROTO_IP:
      switch (optname) {

#if LWIP_NETBUF_RECVINFO
        case IP_RECVORIGDSTADDR:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, *optlen, int, NETCONN_UDP);
          *(int *)optval = uk_load_n(&sock->receive_original_destination);
          break;
#endif

        case IP_TTL:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, int);
          *(int *)optval = sock->conn->pcb.ip->ttl;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, IP_TTL) = %d\n",
                                      s, *(int *)optval));
          break;
        case IP_TOS:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, int);
          *(int *)optval = sock->conn->pcb.ip->tos;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, IP_TOS) = %d\n",
                                      s, *(int *)optval));
          break;
#if LWIP_IPV4 && LWIP_MULTICAST_TX_OPTIONS && LWIP_UDP
        case IP_MULTICAST_TTL:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, u8_t);
          if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_UDP) {
            done_socket(sock);
            return ENOPROTOOPT;
          }
          *(u8_t *)optval = udp_get_multicast_ttl(sock->conn->pcb.udp);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, IP_MULTICAST_TTL) = %d\n",
                                      s, *(int *)optval));
          break;
        case IP_MULTICAST_IF:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, struct in_addr);
          if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_UDP) {
            done_socket(sock);
            return ENOPROTOOPT;
          }
          inet_addr_from_ip4addr((struct in_addr *)optval, udp_get_multicast_netif_addr(sock->conn->pcb.udp));
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, IP_MULTICAST_IF) = 0x%"X32_F"\n",
                                      s, *(u32_t *)optval));
          break;
        case IP_MULTICAST_LOOP:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, u8_t);
          if ((sock->conn->pcb.udp->flags & UDP_FLAGS_MULTICAST_LOOP) != 0) {
            *(u8_t *)optval = 1;
          } else {
            *(u8_t *)optval = 0;
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, IP_MULTICAST_LOOP) = %d\n",
                                      s, *(int *)optval));
          break;
#endif /* LWIP_IPV4 && LWIP_MULTICAST_TX_OPTIONS && LWIP_UDP */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IP, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;

#if LWIP_TCP
    /* Level: IPPROTO_TCP */
    case IPPROTO_TCP:
      /* Special case: all IPPROTO_TCP option take an int */
      LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, *optlen, int, NETCONN_TCP);
      if (sock->conn->pcb.tcp->state == LISTEN) {
        done_socket(sock);
        return EINVAL;
      }
      switch (optname) {
        case TCP_NODELAY:
          *(int *)optval = tcp_nagle_disabled(sock->conn->pcb.tcp);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, TCP_NODELAY) = %s\n",
                                      s, (*(int *)optval) ? "on" : "off") );
          break;
        case TCP_KEEPALIVE:
          *(int *)optval = (int)sock->conn->pcb.tcp->keep_idle;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, TCP_KEEPALIVE) = %d\n",
                                      s, *(int *)optval));
          break;

#if LWIP_TCP_KEEPALIVE
        case TCP_KEEPIDLE:
          *(int *)optval = (int)(sock->conn->pcb.tcp->keep_idle / 1000);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, TCP_KEEPIDLE) = %d\n",
                                      s, *(int *)optval));
          break;
        case TCP_KEEPINTVL:
          *(int *)optval = (int)(sock->conn->pcb.tcp->keep_intvl / 1000);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, TCP_KEEPINTVL) = %d\n",
                                      s, *(int *)optval));
          break;
        case TCP_KEEPCNT:
          *(int *)optval = (int)sock->conn->pcb.tcp->keep_cnt;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, TCP_KEEPCNT) = %d\n",
                                      s, *(int *)optval));
          break;
#endif /* LWIP_TCP_KEEPALIVE */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_TCP, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_TCP */

#if LWIP_IPV6
    /* Level: IPPROTO_IPV6 */
    case IPPROTO_IPV6:
      switch (optname) {
        case IPV6_V6ONLY:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, *optlen, int);
          *(int *)optval = (netconn_get_ipv6only(sock->conn) ? 1 : 0);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IPV6, IPV6_V6ONLY) = %d\n",
                                      s, *(int *)optval));
          break;
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_IPV6, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_IPV6 */

#if LWIP_UDP && LWIP_UDPLITE
    /* Level: IPPROTO_UDPLITE */
    case IPPROTO_UDPLITE:
      /* Special case: all IPPROTO_UDPLITE option take an int */
      LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, *optlen, int);
      /* If this is no UDP lite socket, ignore any options. */
      if (!NETCONNTYPE_ISUDPLITE(netconn_type(sock->conn))) {
        done_socket(sock);
        return ENOPROTOOPT;
      }
      switch (optname) {
        case UDPLITE_SEND_CSCOV:
          *(int *)optval = sock->conn->pcb.udp->chksum_len_tx;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_UDPLITE, UDPLITE_SEND_CSCOV) = %d\n",
                                      s, (*(int *)optval)) );
          break;
        case UDPLITE_RECV_CSCOV:
          *(int *)optval = sock->conn->pcb.udp->chksum_len_rx;
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_UDPLITE, UDPLITE_RECV_CSCOV) = %d\n",
                                      s, (*(int *)optval)) );
          break;
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_UDPLITE, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_UDP */
    /* Level: IPPROTO_RAW */
    case IPPROTO_RAW:
      switch (optname) {
#if LWIP_IPV6 && LWIP_RAW
        case IPV6_CHECKSUM:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, *optlen, int, NETCONN_RAW);
          if (sock->conn->pcb.raw->chksum_reqd == 0) {
            *(int *)optval = -1;
          } else {
            *(int *)optval = sock->conn->pcb.raw->chksum_offset;
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_RAW, IPV6_CHECKSUM) = %d\n",
                                      s, (*(int *)optval)) );
          break;
#endif /* LWIP_IPV6 && LWIP_RAW */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, IPPROTO_RAW, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
    default:
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_getsockopt(%d, level=0x%x, UNIMPL: optname=0x%x, ..)\n",
                                  s, level, optname));
      err = ENOPROTOOPT;
      break;
  } /* switch (level) */

  if (!err) {
    option_length = LWIP_MIN(option_length, *optlen);
    if (uk_nofault_memcpy((char *)user_value, (const char *)&option_value,
                          option_length, 0) != option_length) {
      err = EFAULT;
    } else {
      *optlen = (socklen_t)option_length;
    }
  }
  done_socket(sock);
  return err;
}

int
lwip_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen)
{
  int err = 0;
  struct lwip_sock *sock = get_socket(s);
#if !LWIP_TCPIP_CORE_LOCKING
  err_t cberr;
  LWIP_SETGETSOCKOPT_DATA_VAR_DECLARE(data);
#endif /* !LWIP_TCPIP_CORE_LOCKING */

  if (!sock) {
    return -1;
  }

#if LWIP_TCPIP_CORE_LOCKING
  /* core-locking can just call the -impl function */
  LOCK_TCPIP_CORE();
  err = lwip_setsockopt_impl(s, level, optname, optval, optlen);
  UNLOCK_TCPIP_CORE();

#else /* LWIP_TCPIP_CORE_LOCKING */

#if LWIP_MPU_COMPATIBLE
  /* MPU_COMPATIBLE copies the optval data, so check for max size here */
  if (optlen > LWIP_SETGETSOCKOPT_MAXOPTLEN) {
    sock_set_errno(sock, ENOBUFS);
    done_socket(sock);
    return -1;
  }
#endif /* LWIP_MPU_COMPATIBLE */

  LWIP_SETGETSOCKOPT_DATA_VAR_ALLOC(data, sock);
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).s = s;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).level = level;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optname = optname;
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optlen = optlen;
#if LWIP_MPU_COMPATIBLE
  MEMCPY(LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optval, optval, optlen);
#else /* LWIP_MPU_COMPATIBLE */
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).optval.pc = (const void *)optval;
#endif /* LWIP_MPU_COMPATIBLE */
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).err = 0;
#if LWIP_NETCONN_SEM_PER_THREAD
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem = LWIP_NETCONN_THREAD_SEM_GET();
#else
  LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem = &sock->conn->op_completed;
#endif
  cberr = tcpip_callback(lwip_setsockopt_callback, &LWIP_SETGETSOCKOPT_DATA_VAR_REF(data));
  if (cberr != ERR_OK) {
    LWIP_SETGETSOCKOPT_DATA_VAR_FREE(data);
    sock_set_errno(sock, err_to_errno(cberr));
    done_socket(sock);
    return -1;
  }
  sys_arch_sem_wait((sys_sem_t *)(LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).completed_sem), 0);

  /* maybe lwip_getsockopt_internal has changed err */
  err = LWIP_SETGETSOCKOPT_DATA_VAR_REF(data).err;
  LWIP_SETGETSOCKOPT_DATA_VAR_FREE(data);
#endif  /* LWIP_TCPIP_CORE_LOCKING */

  sock_set_errno(sock, err);
  done_socket(sock);
  return err ? -1 : 0;
}

#if !LWIP_TCPIP_CORE_LOCKING
/** lwip_setsockopt_callback: only used without CORE_LOCKING
 * to get into the tcpip_thread
 */
static void
lwip_setsockopt_callback(void *arg)
{
  struct lwip_setgetsockopt_data *data;
  LWIP_ASSERT("arg != NULL", arg != NULL);
  data = (struct lwip_setgetsockopt_data *)arg;

  data->err = lwip_setsockopt_impl(data->s, data->level, data->optname,
#if LWIP_MPU_COMPATIBLE
                                   data->optval,
#else /* LWIP_MPU_COMPATIBLE */
                                   data->optval.pc,
#endif /* LWIP_MPU_COMPATIBLE */
                                   data->optlen);

  sys_sem_signal((sys_sem_t *)(data->completed_sem));
}
#endif  /* LWIP_TCPIP_CORE_LOCKING */

/** lwip_setsockopt_impl: the actual implementation of setsockopt:
 * same argument as lwip_setsockopt, either called directly or through callback
 */
static int
lwip_setsockopt_impl(int s, int level, int optname, const void *optval, socklen_t optlen)
{
  int err = 0;
  const int option_input = 1;
  union lwip_sockopt_value option_value = {0};
  size_t option_length = 0;
  const void *user_value = optval;
  struct lwip_sock *sock = tryget_socket(s);
  if (!sock) {
    return EBADF;
  }

#ifdef LWIP_HOOK_SOCKETS_SETSOCKOPT
  if (LWIP_HOOK_SOCKETS_SETSOCKOPT(s, sock, level, optname, optval, optlen, &err)) {
    return err;
  }
#endif

  if (level == SOL_SOCKET && optname != SO_BINDTODEVICE) {
    if (optlen < sizeof(int)) {
      done_socket(sock);
      return EINVAL;
    }
    if (!uk_nofault_try_read(option_value.integer, user_value)) {
      done_socket(sock);
      return EFAULT;
    }
  }
  optval = &option_value;

  switch (level) {

    /* Level: SOL_SOCKET */
    case SOL_SOCKET:
      switch (optname) {

        case SO_TIMESTAMP:
        case SO_TIMESTAMPNS:
        case SO_TIMESTAMP_NEW:
        case SO_TIMESTAMPNS_NEW:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, int);
          uk_store_n(&sock->timestamp_mode, *(const int *)optval ? optname : 0);
          break;


        /* SO_ACCEPTCONN is get-only */

        /* The option flags */
        case SO_BROADCAST:
        case SO_KEEPALIVE:
#if SO_REUSE
        case SO_REUSEADDR:
#endif /* SO_REUSE */
          if ((optname == SO_BROADCAST) &&
              (NETCONNTYPE_GROUP(sock->conn->type) != NETCONN_UDP)) {
            done_socket(sock);
            return ENOPROTOOPT;
          }

          optname = lwip_sockopt_to_ipopt(optname);

          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, int);
          if (*(const int *)optval) {
            ip_set_option(sock->conn->pcb.ip, optname);
          } else {
            ip_reset_option(sock->conn->pcb.ip, optname);
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, SOL_SOCKET, optname=0x%x, ..) -> %s\n",
                                      s, optname, (*(const int *)optval ? "on" : "off")));
          break;

          /* SO_TYPE is get-only */
          /* SO_ERROR is get-only */

#if LWIP_SO_SNDTIMEO
        case SO_SNDTIMEO: {
          long ms_long;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, LWIP_SO_SNDRCVTIMEO_OPTTYPE);
          ms_long = LWIP_SO_SNDRCVTIMEO_GET_MS(optval);
          if (ms_long < 0) {
            done_socket(sock);
            return EINVAL;
          }
          netconn_set_sendtimeout(sock->conn, ms_long);
          break;
        }
#endif /* LWIP_SO_SNDTIMEO */
#if LWIP_SO_RCVTIMEO
        case SO_RCVTIMEO: {
          long ms_long;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, LWIP_SO_SNDRCVTIMEO_OPTTYPE);
          ms_long = LWIP_SO_SNDRCVTIMEO_GET_MS(optval);
          if (ms_long < 0) {
            done_socket(sock);
            return EINVAL;
          }
          netconn_set_recvtimeout(sock->conn, (u32_t)ms_long);
          break;
        }
#endif /* LWIP_SO_RCVTIMEO */
#if LWIP_TCP
        case SO_SNDBUF: {
          u32_t maximum = LWIP_MIN((u32_t)TCP_SND_BUF / 2, (u32_t)INT_MAX / 2);
          u32_t requested;

          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, int);
          if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
            done_socket(sock);
            return ENOPROTOOPT;
          }
          requested = (u32_t)*(const int *)optval;
          requested = LWIP_MAX(4608, 2 * LWIP_MIN(requested, maximum));
          lwip_netconn_set_sendbufsize(sock->conn, requested);
          break;
        }
#endif /* LWIP_TCP */
#if LWIP_SO_RCVBUF
        case SO_RCVBUF:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, int);
          netconn_set_recvbufsize(sock->conn, *(const int *)optval);
          break;
#endif /* LWIP_SO_RCVBUF */
#if LWIP_SO_LINGER
        case SO_LINGER: {
          const struct linger *linger = (const struct linger *)optval;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, struct linger);
          if (linger->l_onoff) {
            int lingersec = linger->l_linger;
            if (lingersec < 0) {
              done_socket(sock);
              return EINVAL;
            }
            if (lingersec > 0xFFFF) {
              lingersec = 0xFFFF;
            }
            sock->conn->linger = (s16_t)lingersec;
          } else {
            sock->conn->linger = -1;
          }
        }
        break;
#endif /* LWIP_SO_LINGER */
#if LWIP_UDP
        case SO_NO_CHECK:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, int, NETCONN_UDP);
#if LWIP_UDPLITE
          if (udp_is_flag_set(sock->conn->pcb.udp, UDP_FLAGS_UDPLITE)) {
            /* this flag is only available for UDP, not for UDP lite */
            done_socket(sock);
            return EAFNOSUPPORT;
          }
#endif /* LWIP_UDPLITE */
          if (*(const int *)optval) {
            udp_set_flags(sock->conn->pcb.udp, UDP_FLAGS_NOCHKSUM);
          } else {
            udp_clear_flags(sock->conn->pcb.udp, UDP_FLAGS_NOCHKSUM);
          }
          break;
#endif /* LWIP_UDP */
        case SO_BINDTODEVICE: {
          const struct ifreq *iface;
          struct netif *n = NULL;

          LWIP_SOCKOPT_CHECK_OPTLEN_CONN(sock, optlen, struct ifreq);

          iface = (const struct ifreq *)optval;
          if (iface->ifr_name[0] != 0) {
            n = netif_find(iface->ifr_name);
            if (n == NULL) {
              done_socket(sock);
              return ENODEV;
            }
          }

          switch (NETCONNTYPE_GROUP(netconn_type(sock->conn))) {
#if LWIP_TCP
            case NETCONN_TCP:
              tcp_bind_netif(sock->conn->pcb.tcp, n);
              break;
#endif
#if LWIP_UDP
            case NETCONN_UDP:
              udp_bind_netif(sock->conn->pcb.udp, n);
              break;
#endif
#if LWIP_RAW
            case NETCONN_RAW:
              raw_bind_netif(sock->conn->pcb.raw, n);
              break;
#endif
            default:
              LWIP_ASSERT("Unhandled netconn type in SO_BINDTODEVICE", 0);
              break;
          }
        }
        break;
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, SOL_SOCKET, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;

    /* Level: IPPROTO_IP */
    case IPPROTO_IP:
      switch (optname) {

#if LWIP_NETBUF_RECVINFO
        case IP_RECVORIGDSTADDR:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, int, NETCONN_UDP);
          uk_store_n(&sock->receive_original_destination, !!*(const int *)optval);
          break;
#endif

        case IP_TTL:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, int);
          sock->conn->pcb.ip->ttl = (u8_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_IP, IP_TTL, ..) -> %d\n",
                                      s, sock->conn->pcb.ip->ttl));
          break;
        case IP_TOS:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, int);
          sock->conn->pcb.ip->tos = (u8_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_IP, IP_TOS, ..)-> %d\n",
                                      s, sock->conn->pcb.ip->tos));
          break;
#if LWIP_NETBUF_RECVINFO
        case IP_PKTINFO:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, int, NETCONN_UDP);
          if (*(const int *)optval) {
            sock->conn->flags |= NETCONN_FLAG_PKTINFO;
          } else {
            sock->conn->flags &= ~NETCONN_FLAG_PKTINFO;
          }
          break;
#endif /* LWIP_NETBUF_RECVINFO */
#if LWIP_IPV4 && LWIP_MULTICAST_TX_OPTIONS && LWIP_UDP
        case IP_MULTICAST_TTL:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, u8_t, NETCONN_UDP);
          udp_set_multicast_ttl(sock->conn->pcb.udp, (u8_t)(*(const u8_t *)optval));
          break;
        case IP_MULTICAST_IF: {
          ip4_addr_t if_addr;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, struct in_addr, NETCONN_UDP);
          inet_addr_to_ip4addr(&if_addr, (const struct in_addr *)optval);
          udp_set_multicast_netif_addr(sock->conn->pcb.udp, &if_addr);
        }
        break;
        case IP_MULTICAST_LOOP:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, u8_t, NETCONN_UDP);
          if (*(const u8_t *)optval) {
            udp_set_flags(sock->conn->pcb.udp, UDP_FLAGS_MULTICAST_LOOP);
          } else {
            udp_clear_flags(sock->conn->pcb.udp, UDP_FLAGS_MULTICAST_LOOP);
          }
          break;
#endif /* LWIP_IPV4 && LWIP_MULTICAST_TX_OPTIONS && LWIP_UDP */
#if LWIP_IGMP
        case IP_ADD_MEMBERSHIP:
        case IP_DROP_MEMBERSHIP: {
          /* If this is a TCP or a RAW socket, ignore these options. */
          err_t igmp_err;
          const struct ip_mreq *imr = (const struct ip_mreq *)optval;
          ip4_addr_t if_addr;
          ip4_addr_t multi_addr;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, struct ip_mreq, NETCONN_UDP);
          inet_addr_to_ip4addr(&if_addr, &imr->imr_interface);
          inet_addr_to_ip4addr(&multi_addr, &imr->imr_multiaddr);
          if (optname == IP_ADD_MEMBERSHIP) {
            if (!lwip_socket_register_membership(s, &if_addr, &multi_addr)) {
              /* cannot track membership (out of memory) */
              err = ENOMEM;
              igmp_err = ERR_OK;
            } else {
              igmp_err = igmp_joingroup(&if_addr, &multi_addr);
            }
          } else {
            igmp_err = igmp_leavegroup(&if_addr, &multi_addr);
            lwip_socket_unregister_membership(s, &if_addr, &multi_addr);
          }
          if (igmp_err != ERR_OK) {
            err = EADDRNOTAVAIL;
          }
        }
        break;
#endif /* LWIP_IGMP */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_IP, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;

#if LWIP_TCP
    /* Level: IPPROTO_TCP */
    case IPPROTO_TCP:
      /* Special case: all IPPROTO_TCP option take an int */
      LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, int, NETCONN_TCP);
      if (sock->conn->pcb.tcp->state == LISTEN) {
        done_socket(sock);
        return EINVAL;
      }
      switch (optname) {
        case TCP_NODELAY:
          if (*(const int *)optval) {
            tcp_nagle_disable(sock->conn->pcb.tcp);
          } else {
            tcp_nagle_enable(sock->conn->pcb.tcp);
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, TCP_NODELAY) -> %s\n",
                                      s, (*(const int *)optval) ? "on" : "off") );
          break;
        case TCP_KEEPALIVE:
          sock->conn->pcb.tcp->keep_idle = (u32_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, TCP_KEEPALIVE) -> %"U32_F"\n",
                                      s, sock->conn->pcb.tcp->keep_idle));
          break;

#if LWIP_TCP_KEEPALIVE
        case TCP_KEEPIDLE:
          sock->conn->pcb.tcp->keep_idle = 1000 * (u32_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, TCP_KEEPIDLE) -> %"U32_F"\n",
                                      s, sock->conn->pcb.tcp->keep_idle));
          break;
        case TCP_KEEPINTVL:
          sock->conn->pcb.tcp->keep_intvl = 1000 * (u32_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, TCP_KEEPINTVL) -> %"U32_F"\n",
                                      s, sock->conn->pcb.tcp->keep_intvl));
          break;
        case TCP_KEEPCNT:
          sock->conn->pcb.tcp->keep_cnt = (u32_t)(*(const int *)optval);
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, TCP_KEEPCNT) -> %"U32_F"\n",
                                      s, sock->conn->pcb.tcp->keep_cnt));
          break;
#endif /* LWIP_TCP_KEEPALIVE */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_TCP, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_TCP*/

#if LWIP_IPV6
    /* Level: IPPROTO_IPV6 */
    case IPPROTO_IPV6:
      switch (optname) {
        case IPV6_V6ONLY:
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, int);
          if (*(const int *)optval) {
            netconn_set_ipv6only(sock->conn, 1);
          } else {
            netconn_set_ipv6only(sock->conn, 0);
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_IPV6, IPV6_V6ONLY, ..) -> %d\n",
                                      s, (netconn_get_ipv6only(sock->conn) ? 1 : 0)));
          break;
#if LWIP_IPV6_MLD
        case IPV6_JOIN_GROUP:
        case IPV6_LEAVE_GROUP: {
          /* If this is a TCP or a RAW socket, ignore these options. */
          err_t mld6_err;
          struct netif *netif;
          ip6_addr_t multi_addr;
          const struct ipv6_mreq *imr = (const struct ipv6_mreq *)optval;
          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, struct ipv6_mreq, NETCONN_UDP);
          inet6_addr_to_ip6addr(&multi_addr, &imr->ipv6mr_multiaddr);
          LWIP_ASSERT("Invalid netif index", imr->ipv6mr_interface <= 0xFFu);
          netif = netif_get_by_index((u8_t)imr->ipv6mr_interface);
          if (netif == NULL) {
            err = EADDRNOTAVAIL;
            break;
          }

          if (optname == IPV6_JOIN_GROUP) {
            if (!lwip_socket_register_mld6_membership(s, imr->ipv6mr_interface, &multi_addr)) {
              /* cannot track membership (out of memory) */
              err = ENOMEM;
              mld6_err = ERR_OK;
            } else {
              mld6_err = mld6_joingroup_netif(netif, &multi_addr);
            }
          } else {
            mld6_err = mld6_leavegroup_netif(netif, &multi_addr);
            lwip_socket_unregister_mld6_membership(s, imr->ipv6mr_interface, &multi_addr);
          }
          if (mld6_err != ERR_OK) {
            err = EADDRNOTAVAIL;
          }
        }
        break;
#endif /* LWIP_IPV6_MLD */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_IPV6, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_IPV6 */

#if LWIP_UDP && LWIP_UDPLITE
    /* Level: IPPROTO_UDPLITE */
    case IPPROTO_UDPLITE:
      /* Special case: all IPPROTO_UDPLITE option take an int */
      LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB(sock, optlen, int);
      /* If this is no UDP lite socket, ignore any options. */
      if (!NETCONNTYPE_ISUDPLITE(netconn_type(sock->conn))) {
        done_socket(sock);
        return ENOPROTOOPT;
      }
      switch (optname) {
        case UDPLITE_SEND_CSCOV:
          if ((*(const int *)optval != 0) && ((*(const int *)optval < 8) || (*(const int *)optval > 0xffff))) {
            /* don't allow illegal values! */
            sock->conn->pcb.udp->chksum_len_tx = 8;
          } else {
            sock->conn->pcb.udp->chksum_len_tx = (u16_t) * (const int *)optval;
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_UDPLITE, UDPLITE_SEND_CSCOV) -> %d\n",
                                      s, (*(const int *)optval)) );
          break;
        case UDPLITE_RECV_CSCOV:
          if ((*(const int *)optval != 0) && ((*(const int *)optval < 8) || (*(const int *)optval > 0xffff))) {
            /* don't allow illegal values! */
            sock->conn->pcb.udp->chksum_len_rx = 8;
          } else {
            sock->conn->pcb.udp->chksum_len_rx = (u16_t) * (const int *)optval;
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_UDPLITE, UDPLITE_RECV_CSCOV) -> %d\n",
                                      s, (*(const int *)optval)) );
          break;
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_UDPLITE, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
#endif /* LWIP_UDP */
    /* Level: IPPROTO_RAW */
    case IPPROTO_RAW:
      switch (optname) {
#if LWIP_IPV6 && LWIP_RAW
        case IPV6_CHECKSUM:
          /* It should not be possible to disable the checksum generation with ICMPv6
           * as per RFC 3542 chapter 3.1 */
          if (sock->conn->pcb.raw->protocol == IPPROTO_ICMPV6) {
            done_socket(sock);
            return EINVAL;
          }

          LWIP_SOCKOPT_CHECK_OPTLEN_CONN_PCB_TYPE(sock, optlen, int, NETCONN_RAW);
          if (*(const int *)optval < 0) {
            sock->conn->pcb.raw->chksum_reqd = 0;
          } else if (*(const int *)optval & 1) {
            /* Per RFC3542, odd offsets are not allowed */
            done_socket(sock);
            return EINVAL;
          } else {
            sock->conn->pcb.raw->chksum_reqd = 1;
            sock->conn->pcb.raw->chksum_offset = (u16_t) * (const int *)optval;
          }
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_RAW, IPV6_CHECKSUM, ..) -> %d\n",
                                      s, sock->conn->pcb.raw->chksum_reqd));
          break;
#endif /* LWIP_IPV6 && LWIP_RAW */
        default:
          LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, IPPROTO_RAW, UNIMPL: optname=0x%x, ..)\n",
                                      s, optname));
          err = ENOPROTOOPT;
          break;
      }  /* switch (optname) */
      break;
    default:
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_setsockopt(%d, level=0x%x, UNIMPL: optname=0x%x, ..)\n",
                                  s, level, optname));
      err = ENOPROTOOPT;
      break;
  }  /* switch (level) */

  done_socket(sock);
  return err;
}

int
lwip_ioctl(int s, long cmd, void *argp)
{
  struct lwip_sock *sock = get_socket(s);
  u8_t val;
#if LWIP_SO_RCVBUF
  int recv_avail;
#endif /* LWIP_SO_RCVBUF */

  if (!sock) {
    return -1;
  }

  switch (cmd) {
#if LWIP_SO_RCVBUF || LWIP_FIONREAD_LINUXMODE
    case FIONREAD:
      if (!argp) {
        sock_set_errno(sock, EINVAL);
        done_socket(sock);
        return -1;
      }
#if LWIP_FIONREAD_LINUXMODE
      if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
        struct netbuf *nb;
        if (sock->lastdata.netbuf) {
          nb = sock->lastdata.netbuf;
          *((int *)argp) = nb->p->tot_len;
        } else {
          struct netbuf *rxbuf;
          err_t err = netconn_recv_udp_raw_netbuf_flags(sock->conn, &rxbuf, NETCONN_DONTBLOCK);
          if (err != ERR_OK) {
            *((int *)argp) = 0;
          } else {
            sock->lastdata.netbuf = rxbuf;
            *((int *)argp) = rxbuf->p->tot_len;
          }
        }
        done_socket(sock);
        return 0;
      }
#endif /* LWIP_FIONREAD_LINUXMODE */

#if LWIP_SO_RCVBUF
      /* we come here if either LWIP_FIONREAD_LINUXMODE==0 or this is a TCP socket */
      SYS_ARCH_GET(sock->conn->recv_avail, recv_avail);
      if (recv_avail < 0) {
        recv_avail = 0;
      }

      /* Check if there is data left from the last recv operation. /maq 041215 */
      if (sock->lastdata.netbuf) {
        if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
          struct lwip_socket_packet *packet = sock->lastdata.tcp;

          while (packet) {
            recv_avail += LWIP_MIN((int)packet->buffer->tot_len,
                                  INT_MAX - recv_avail);
            packet = packet->next;
          }
        } else {
          recv_avail += sock->lastdata.netbuf->p->tot_len;
        }
      }
      *((int *)argp) = recv_avail;

      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_ioctl(%d, FIONREAD, %p) = %"U16_F"\n", s, argp, *((u16_t *)argp)));
      sock_set_errno(sock, 0);
      done_socket(sock);
      return 0;
#else /* LWIP_SO_RCVBUF */
      break;
#endif /* LWIP_SO_RCVBUF */
#endif /* LWIP_SO_RCVBUF || LWIP_FIONREAD_LINUXMODE */

    case (long)FIONBIO:
      val = 0;
      if (argp && *(int *)argp) {
        val = 1;
      }
      netconn_set_nonblocking(sock->conn, val);
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_ioctl(%d, FIONBIO, %d)\n", s, val));
      sock_set_errno(sock, 0);
      done_socket(sock);
      return 0;

    default:
      break;
  } /* switch (cmd) */
  LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_ioctl(%d, UNIMPL: 0x%lx, %p)\n", s, cmd, argp));
  sock_set_errno(sock, ENOSYS); /* not yet implemented */
  done_socket(sock);
  return -1;
}

/** A minimal implementation of fcntl.
 * Currently only the commands F_GETFL and F_SETFL are implemented.
 * The flag O_NONBLOCK and access modes are supported for F_GETFL, only
 * the flag O_NONBLOCK is implemented for F_SETFL.
 */
int
lwip_fcntl(int s, int cmd, int val)
{
  struct lwip_sock *sock = get_socket(s);
  int ret = -1;
  int op_mode = 0;

  if (!sock) {
    return -1;
  }

  switch (cmd) {
    case F_GETFL:
      ret = netconn_is_nonblocking(sock->conn) ? O_NONBLOCK : 0;
      sock_set_errno(sock, 0);

      if (NETCONNTYPE_GROUP(netconn_type(sock->conn)) == NETCONN_TCP) {
#if LWIP_TCPIP_CORE_LOCKING
        LOCK_TCPIP_CORE();
#else
        SYS_ARCH_DECL_PROTECT(lev);
        /* the proper thing to do here would be to get into the tcpip_thread,
           but locking should be OK as well since we only *read* some flags */
        SYS_ARCH_PROTECT(lev);
#endif
#if LWIP_TCP
        if (sock->conn->pcb.tcp) {
          if (!(sock->conn->pcb.tcp->flags & TF_RXCLOSED)) {
            op_mode |= O_RDONLY;
          }
          if (!(sock->conn->pcb.tcp->flags & TF_FIN)) {
            op_mode |= O_WRONLY;
          }
        }
#endif
#if LWIP_TCPIP_CORE_LOCKING
        UNLOCK_TCPIP_CORE();
#else
        SYS_ARCH_UNPROTECT(lev);
#endif
      } else {
        op_mode |= O_RDWR;
      }

      /* ensure O_RDWR for (O_RDONLY|O_WRONLY) != O_RDWR cases */
      ret |= (op_mode == (O_RDONLY | O_WRONLY)) ? O_RDWR : op_mode;

      break;
    case F_SETFL:
      /* Bits corresponding to the file access mode and the file creation flags [..] that are set in arg shall be ignored */
      val &= ~(O_RDONLY | O_WRONLY | O_RDWR);
      if ((val & ~O_NONBLOCK) == 0) {
        /* only O_NONBLOCK, all other bits are zero */
        netconn_set_nonblocking(sock->conn, val & O_NONBLOCK);
        ret = 0;
        sock_set_errno(sock, 0);
      } else {
        sock_set_errno(sock, ENOSYS); /* not yet implemented */
      }
      break;
    default:
      LWIP_DEBUGF(SOCKETS_DEBUG, ("lwip_fcntl(%d, UNIMPL: %d, %d)\n", s, cmd, val));
      sock_set_errno(sock, ENOSYS); /* not yet implemented */
      break;
  }
  done_socket(sock);
  return ret;
}

#if LWIP_COMPAT_SOCKETS == 2 && LWIP_POSIX_SOCKETS_IO_NAMES
int
fcntl(int s, int cmd, ...)
{
  va_list ap;
  int val;

  va_start(ap, cmd);
  val = va_arg(ap, int);
  va_end(ap);
  return lwip_fcntl(s, cmd, val);
}
#endif

const char *
lwip_inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
  const char *ret = NULL;
  int size_int = (int)size;
  if (size_int < 0) {
    set_errno(ENOSPC);
    return NULL;
  }
  switch (af) {
#if LWIP_IPV4
    case AF_INET:
      ret = ip4addr_ntoa_r((const ip4_addr_t *)src, dst, size_int);
      if (ret == NULL) {
        set_errno(ENOSPC);
      }
      break;
#endif
#if LWIP_IPV6
    case AF_INET6:
      ret = ip6addr_ntoa_r((const ip6_addr_t *)src, dst, size_int);
      if (ret == NULL) {
        set_errno(ENOSPC);
      }
      break;
#endif
    default:
      set_errno(EAFNOSUPPORT);
      break;
  }
  return ret;
}

int
lwip_inet_pton(int af, const char *src, void *dst)
{
  int err;
  switch (af) {
#if LWIP_IPV4
    case AF_INET:
      err = ip4addr_aton(src, (ip4_addr_t *)dst);
      break;
#endif
#if LWIP_IPV6
    case AF_INET6: {
      /* convert into temporary variable since ip6_addr_t might be larger
         than in6_addr when scopes are enabled */
      ip6_addr_t addr;
      err = ip6addr_aton(src, &addr);
      if (err) {
        memcpy(dst, &addr.addr, sizeof(addr.addr));
      }
      break;
    }
#endif
    default:
      err = -1;
      set_errno(EAFNOSUPPORT);
      break;
  }
  return err;
}

#if LWIP_IGMP
/** Register a new IGMP membership. On socket close, the membership is dropped automatically.
 *
 * ATTENTION: this function is called from tcpip_thread (or under CORE_LOCK).
 *
 * @return 1 on success, 0 on failure
 */
static int
lwip_socket_register_membership(int s, const ip4_addr_t *if_addr, const ip4_addr_t *multi_addr)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return 0;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if (socket_ipv4_multicast_memberships[i].sock == NULL) {
      socket_ipv4_multicast_memberships[i].sock = sock;
      ip4_addr_copy(socket_ipv4_multicast_memberships[i].if_addr, *if_addr);
      ip4_addr_copy(socket_ipv4_multicast_memberships[i].multi_addr, *multi_addr);
      done_socket(sock);
      return 1;
    }
  }
  done_socket(sock);
  return 0;
}

/** Unregister a previously registered membership. This prevents dropping the membership
 * on socket close.
 *
 * ATTENTION: this function is called from tcpip_thread (or under CORE_LOCK).
 */
static void
lwip_socket_unregister_membership(int s, const ip4_addr_t *if_addr, const ip4_addr_t *multi_addr)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if ((socket_ipv4_multicast_memberships[i].sock == sock) &&
        ip4_addr_cmp(&socket_ipv4_multicast_memberships[i].if_addr, if_addr) &&
        ip4_addr_cmp(&socket_ipv4_multicast_memberships[i].multi_addr, multi_addr)) {
      socket_ipv4_multicast_memberships[i].sock = NULL;
      ip4_addr_set_zero(&socket_ipv4_multicast_memberships[i].if_addr);
      ip4_addr_set_zero(&socket_ipv4_multicast_memberships[i].multi_addr);
      break;
    }
  }
  done_socket(sock);
}

/** Drop all memberships of a socket that were not dropped explicitly via setsockopt.
 *
 * ATTENTION: this function is NOT called from tcpip_thread (or under CORE_LOCK).
 */
static void
lwip_socket_drop_registered_memberships(int s)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if (socket_ipv4_multicast_memberships[i].sock == sock) {
      ip_addr_t multi_addr, if_addr;
      ip_addr_copy_from_ip4(multi_addr, socket_ipv4_multicast_memberships[i].multi_addr);
      ip_addr_copy_from_ip4(if_addr, socket_ipv4_multicast_memberships[i].if_addr);
      socket_ipv4_multicast_memberships[i].sock = NULL;
      ip4_addr_set_zero(&socket_ipv4_multicast_memberships[i].if_addr);
      ip4_addr_set_zero(&socket_ipv4_multicast_memberships[i].multi_addr);

      netconn_join_leave_group(sock->conn, &multi_addr, &if_addr, NETCONN_LEAVE);
    }
  }
  done_socket(sock);
}
#endif /* LWIP_IGMP */

#if LWIP_IPV6_MLD
/** Register a new MLD6 membership. On socket close, the membership is dropped automatically.
 *
 * ATTENTION: this function is called from tcpip_thread (or under CORE_LOCK).
 *
 * @return 1 on success, 0 on failure
 */
static int
lwip_socket_register_mld6_membership(int s, unsigned int if_idx, const ip6_addr_t *multi_addr)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return 0;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if (socket_ipv6_multicast_memberships[i].sock == NULL) {
      socket_ipv6_multicast_memberships[i].sock   = sock;
      socket_ipv6_multicast_memberships[i].if_idx = (u8_t)if_idx;
      ip6_addr_copy(socket_ipv6_multicast_memberships[i].multi_addr, *multi_addr);
      done_socket(sock);
      return 1;
    }
  }
  done_socket(sock);
  return 0;
}

/** Unregister a previously registered MLD6 membership. This prevents dropping the membership
 * on socket close.
 *
 * ATTENTION: this function is called from tcpip_thread (or under CORE_LOCK).
 */
static void
lwip_socket_unregister_mld6_membership(int s, unsigned int if_idx, const ip6_addr_t *multi_addr)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if ((socket_ipv6_multicast_memberships[i].sock   == sock) &&
        (socket_ipv6_multicast_memberships[i].if_idx == if_idx) &&
        ip6_addr_cmp(&socket_ipv6_multicast_memberships[i].multi_addr, multi_addr)) {
      socket_ipv6_multicast_memberships[i].sock   = NULL;
      socket_ipv6_multicast_memberships[i].if_idx = NETIF_NO_INDEX;
      ip6_addr_set_zero(&socket_ipv6_multicast_memberships[i].multi_addr);
      break;
    }
  }
  done_socket(sock);
}

/** Drop all MLD6 memberships of a socket that were not dropped explicitly via setsockopt.
 *
 * ATTENTION: this function is NOT called from tcpip_thread (or under CORE_LOCK).
 */
static void
lwip_socket_drop_registered_mld6_memberships(int s)
{
  struct lwip_sock *sock = get_socket(s);
  int i;

  if (!sock) {
    return;
  }

  for (i = 0; i < LWIP_SOCKET_MAX_MEMBERSHIPS; i++) {
    if (socket_ipv6_multicast_memberships[i].sock == sock) {
      ip_addr_t multi_addr;
      u8_t if_idx;

      ip_addr_copy_from_ip6(multi_addr, socket_ipv6_multicast_memberships[i].multi_addr);
      if_idx = socket_ipv6_multicast_memberships[i].if_idx;

      socket_ipv6_multicast_memberships[i].sock   = NULL;
      socket_ipv6_multicast_memberships[i].if_idx = NETIF_NO_INDEX;
      ip6_addr_set_zero(&socket_ipv6_multicast_memberships[i].multi_addr);

      netconn_join_leave_group_netif(sock->conn, &multi_addr, if_idx, NETCONN_LEAVE);
    }
  }
  done_socket(sock);
}
#endif /* LWIP_IPV6_MLD */

#endif /* LWIP_SOCKET */
