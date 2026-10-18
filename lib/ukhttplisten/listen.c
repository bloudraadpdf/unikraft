/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <uk/devfs.h>
#include <uk/file.h>
#include <uk/file/nops.h>
#include <uk/fs.h>
#include <uk/fs/prio.h>
#include <uk/init.h>
#include <uk/socket_event.h>

static uint16_t watched_port;
static const struct uk_file listen_file;

static void socket_listening(const struct uk_socket_event *event)
{
	const struct sockaddr_in *address;
	uint16_t port = __atomic_load_n(&watched_port, __ATOMIC_ACQUIRE);

	if (!port || (event->type & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) !=
	    SOCK_STREAM || !event->laddr ||
	    event->laddr_len < sizeof(*address))
		return;
	address = (const struct sockaddr_in *)event->laddr;
	if (address->sin_port == port)
		uk_file_event_set(&listen_file, POLLIN);
}

UK_SOCKET_EVENT_RECEIVER(AF_INET, LISTEN, socket_listening);

static struct uk_file_io_result
listen_watch(const struct uk_file *file __unused, const struct iovec *iov,
	     size_t count, size_t offset __unused, long flags __unused,
	     enum uk_file_write_kind kind __unused)
{
	uint8_t bytes[2];
	uint16_t port, expected = 0;
	size_t copied = 0;

	for (size_t i = 0; i < count; ++i) {
		if (iov[i].iov_len > sizeof(bytes) - copied)
			return uk_file_io_status(-EINVAL);
		memcpy(bytes + copied, iov[i].iov_base, iov[i].iov_len);
		copied += iov[i].iov_len;
	}
	if (copied != sizeof(bytes))
		return uk_file_io_status(-EINVAL);
	memcpy(&port, bytes, sizeof(port));
	if (!port)
		return uk_file_io_status(-EINVAL);
	if (!__atomic_compare_exchange_n(&watched_port, &expected, port, 0,
					__ATOMIC_RELEASE, __ATOMIC_RELAXED))
		return uk_file_io_status(-EBUSY);
	return uk_file_io_status(copied);
}

static int listen_stat(const struct uk_file *file __unused,
		       unsigned int mask __unused, struct uk_statx *stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->stx_mask = UK_STATX_TYPE | UK_STATX_MODE | UK_STATX_NLINK;
	stat->stx_mode = S_IFCHR | 0600;
	stat->stx_nlink = 1;
	return 0;
}

static const struct uk_file_ops listen_ops = {
	.read = uk_file_nop_read,
	.write = listen_watch,
	.mem = uk_file_nop_mem,
	.getstat = listen_stat,
	.setstat = uk_file_nop_setstat,
	.ctl = uk_file_nop_ctl,
};
static uk_file_refcnt listen_ref = UK_FILE_REFCNT_INITIALIZER(listen_ref);
static struct uk_file_state listen_state =
	UK_FILE_STATE_INITIALIZER(listen_state);
static const struct uk_file listen_file = {
	.ops = &listen_ops, .refcnt = &listen_ref, .state = &listen_state,
	._release = uk_file_static_release,
};

static int listen_device_init(struct uk_init_ctx *context __unused)
{
	const union uk_fs_create_target target = { .file = &listen_file };
	const void *created = uk_fs_createat(uk_fs_devfs_root, "http-listen", 11,
					   0600, O_EXCL, target);

	return PTRISERR(created) ? PTR2ERR(created) : 0;
}

uk_rootfs_initcall_prio(listen_device_init, 0, UK_FS_PRIO_FSAVAIL);
