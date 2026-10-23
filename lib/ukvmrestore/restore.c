/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uk/devfs.h>
#include <uk/file.h>
#include <uk/file/nops.h>
#include <uk/fs.h>
#include <uk/fs/prio.h>
#include <uk/init.h>
#include <uk/intctlr.h>
#include <uk/event.h>
#include <uk/libparam.h>
#include <uk/pm.h>
#include <uk/random.h>
#include <uk/spinlock.h>
#include <uk/wait.h>
#if CONFIG_ARCH_ARM_64
#include <uk/bus/platform.h>
#include <uk/ofw/fdt.h>
#include <uk/plat/common/bootinfo.h>
#endif

#define CLOCK_BYTES 112
#define CLOCK_MAGIC 0x4b4c4356
#define CLOCK_FLAGS ((1UL << 8) | (1UL << 9))
#define GENID_REFRESH 0x5600
#define GENID_QUIESCE 0x5601

static __u64 clock_addr;
static __u64 genid_addr;
static unsigned int irq;

UK_LIBPARAM_PARAM(clock_addr, __u64, "VMClock shared page physical address");
UK_LIBPARAM_PARAM(genid_addr, __u64, "VMGenID shared bytes physical address");
UK_LIBPARAM_PARAM(irq, uint, "VMClock notification interrupt");

static __u8 witnessed_id[16];
static __u8 transport_id[16];
static struct uk_waitq transport_wait;
static __u32 read_sequence;
static struct uk_spinlock generation_lock = UK_SPINLOCK_INITIALIZER();
static const struct uk_file clock_file;

static int read_clock(__u8 snapshot[CLOCK_BYTES])
{
	volatile __u32 *seq = (volatile __u32 *)(clock_addr + 12);
	__u32 before, after;

	for (unsigned int attempt = 0; attempt < 256; ++attempt) {
		before = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
		if (before & 1)
			continue;
		memcpy(snapshot, (const void *)clock_addr, CLOCK_BYTES);
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		after = __atomic_load_n(seq, __ATOMIC_RELAXED);
		if (before == after)
			return 0;
	}
	return -EAGAIN;
}

static void read_genid(__u8 id[16])
{
	volatile const __u8 *source = (volatile const __u8 *)genid_addr;

	for (unsigned int i = 0; i < 16; ++i)
		id[i] = source[i];
}

static int same_id(const __u8 left[16], const __u8 right[16])
{
	for (unsigned int i = 0; i < 16; ++i)
		if (left[i] != right[i])
			return 0;
	return 1;
}

static int transport_caught_up(const __u8 id[16])
{
	unsigned long flags;
	int ready;

	uk_spin_lock_irqsave(&generation_lock, flags);
	ready = same_id(id, transport_id);
	uk_spin_unlock_irqrestore(&generation_lock, flags);
	return ready;
}

static int transport_restored(void *argument __unused)
{
	unsigned long flags;

	uk_spin_lock_irqsave(&generation_lock, flags);
	read_genid(transport_id);
	uk_spin_unlock_irqrestore(&generation_lock, flags);
	uk_waitq_wake_up(&transport_wait);
	return UK_EVENT_HANDLED_CONT;
}

UK_EVENT_HANDLER(UK_PM_EVENT_SYSMIGRATION, transport_restored);

static int restore_irq(void *argument __unused)
{
	int rc = uk_random_refresh();

	if (rc)
		UK_CRASH("Snapshot entropy refresh failed (%d)", rc);
	uk_file_event_set(&clock_file, POLLIN);
	return 1;
}

static uk_pollevent clock_poll(const struct uk_file *file __unused,
			      uk_pollevent requested)
{
	volatile __u32 *seq = (volatile __u32 *)(clock_addr + 12);
	__u32 current = __atomic_load_n(seq, __ATOMIC_ACQUIRE);

	if (!(current & 1) &&
	    current != __atomic_load_n(&read_sequence, __ATOMIC_RELAXED))
		return requested & POLLIN;
	return 0;
}

static ssize_t clock_read(const struct uk_file *file __unused,
			  const struct iovec *iov, size_t count,
			  size_t offset, long flags __unused)
{
	__u8 snapshot[CLOCK_BYTES];
	__u32 sequence;
	size_t copied = 0, length;
	int rc;

	if (offset >= CLOCK_BYTES)
		return 0;
	rc = read_clock(snapshot);
	if (rc)
		return rc;
	for (size_t i = 0; i < count && offset < CLOCK_BYTES; ++i) {
		length = MIN(iov[i].iov_len, CLOCK_BYTES - offset);
		memcpy(iov[i].iov_base, snapshot + offset, length);
		offset += length;
		copied += length;
	}
	memcpy(&sequence, snapshot + 12, sizeof(sequence));
	__atomic_store_n(&read_sequence, sequence, __ATOMIC_RELAXED);
	return copied;
}

static int device_stat(const struct uk_file *file, unsigned int mask __unused,
		       struct uk_statx *stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->stx_mask = UK_STATX_TYPE | UK_STATX_MODE | UK_STATX_NLINK |
		UK_STATX_INO | UK_STATX_SIZE;
	stat->stx_mode = S_IFCHR | 0600;
	stat->stx_nlink = 1;
	stat->stx_ino = (file == &clock_file) ? 1 : 2;
	stat->stx_size = (file == &clock_file) ? CLOCK_BYTES : 0;
	stat->stx_blksize = 4096;
	return 0;
}

static int generation_control(const struct uk_file *file __unused,
			      int family, int request,
			      uintptr_t arg1 __unused,
			      uintptr_t arg2 __unused,
			      uintptr_t arg3 __unused)
{
	__u8 id[16];
	unsigned long irq_flags;
	int rc = 0;

	if (family != UKFILE_CTL_IOCTL ||
	    (request != GENID_REFRESH && request != GENID_QUIESCE))
		return -ENOTTY;
	read_genid(id);
	uk_waitq_wait_event(&transport_wait, transport_caught_up(id));
	uk_spin_lock_irqsave(&generation_lock, irq_flags);
	if (request == GENID_REFRESH) {
		read_genid(id);
		if (same_id(id, witnessed_id)) {
			rc = -EALREADY;
		} else {
			rc = uk_random_refresh();
			if (!rc)
				for (unsigned int i = 0; i < 16; ++i)
					witnessed_id[i] = id[i];
		}
	} else {
		rc = uk_random_quiesce();
	}
	uk_spin_unlock_irqrestore(&generation_lock, irq_flags);
	return rc;
}

static off_t clock_seek(const struct uk_file *file __unused, off_t offset,
			int whence, off_t current)
{
	if (whence == SEEK_END)
		current = CLOCK_BYTES;
	else if (whence == SEEK_SET)
		current = 0;
	else if (whence != SEEK_CUR)
		return -EINVAL;
	if (offset > 0 && current > INT64_MAX - offset)
		return -EINVAL;
	if (offset < -current)
		return -EINVAL;
	return current + offset;
}

static const struct uk_file_ops clock_ops = {
	.seek = clock_seek,
	.read = clock_read,
	.write = uk_file_nop_write,
	.mem = uk_file_nop_mem,
	.getstat = device_stat,
	.setstat = uk_file_nop_setstat,
	.ctl = uk_file_nop_ctl,
};
static const struct uk_file_ops generation_ops = {
	.read = uk_file_nop_read,
	.write = uk_file_nop_write,
	.mem = uk_file_nop_mem,
	.getstat = device_stat,
	.setstat = uk_file_nop_setstat,
	.ctl = generation_control,
};

static uk_file_refcnt clock_ref = UK_FILE_REFCNT_INITIALIZER(clock_ref);
static uk_file_refcnt generation_ref = UK_FILE_REFCNT_INITIALIZER(generation_ref);
static struct uk_file_state clock_state =
	UK_FILE_POLLED_STATE_INITIALIZER(clock_state, clock_poll);
static struct uk_file_state generation_state =
	UK_FILE_STATE_INITIALIZER(generation_state);
static const struct uk_file clock_file = {
	.ops = &clock_ops, .refcnt = &clock_ref, .state = &clock_state,
	._release = uk_file_static_release,
};
static const struct uk_file generation_file = {
	.ops = &generation_ops, .refcnt = &generation_ref,
	.state = &generation_state, ._release = uk_file_static_release,
};

#if CONFIG_ARCH_ARM_64
static int discover_restore_devices(void)
{
	const void *fdt = (const void *)ukplat_bootinfo_get()->dtb;
	struct uk_intctlr_irq notification;
	__u64 clock_size, generation_size;
	int clock_node, generation_node, rc;

	if (!fdt || fdt_check_header(fdt))
		return -ENODEV;
	clock_node = fdt_node_offset_by_compatible(fdt, -1, "amazon,vmclock");
	generation_node = fdt_node_offset_by_compatible(fdt, -1, "microsoft,vmgenid");
	if (clock_node < 0 || generation_node < 0)
		return -ENODEV;
	if (fdt_node_offset_by_compatible(fdt, clock_node, "amazon,vmclock") >= 0 ||
	    fdt_node_offset_by_compatible(fdt, generation_node, "microsoft,vmgenid") >= 0)
		return -EINVAL;
	rc = fdt_get_address(fdt, clock_node, 0, &clock_addr, &clock_size);
	if (rc)
		return rc;
	rc = fdt_get_address(fdt, generation_node, 0, &genid_addr, &generation_size);
	if (rc)
		return rc;
	if (clock_size != 4096 || generation_size != 16 ||
	    clock_addr < 0x80000000 || clock_addr > 0x801ff000 || clock_addr % 4096 ||
	    genid_addr < 0x80000000 || genid_addr > 0x801ffff0 || genid_addr % 8 ||
	    (genid_addr < clock_addr + clock_size && clock_addr < genid_addr + generation_size))
		return -EINVAL;
	rc = uk_intctlr_irq_fdt_xlat(fdt, clock_node, 0, &notification);
	if (rc)
		return rc;
	if (notification.trigger != UK_INTCTLR_IRQ_TRIGGER_EDGE)
		return -EINVAL;
	rc = uk_intctlr_irq_configure(&notification);
	if (rc)
		return rc;
	irq = notification.id;
	clock_addr = uk_bus_pf_devmap(clock_addr, clock_size);
	if (PTRISERR(clock_addr))
		return PTR2ERR(clock_addr);
	genid_addr = uk_bus_pf_devmap(genid_addr, generation_size);
	if (PTRISERR(genid_addr))
		return PTR2ERR(genid_addr);
	return 0;
}
#else
static int discover_restore_devices(void)
{
	if (clock_addr < 0x9fc00 || clock_addr > 0xdf000 ||
	    clock_addr % 4096 || genid_addr < 0x9fc00 ||
	    genid_addr > 0xdfff0 || genid_addr % 8 || irq < 5 || irq > 15)
		return -EINVAL;
	return 0;
}
#endif

static int restore_devices_init(struct uk_init_ctx *context __unused)
{
	const union uk_fs_create_target clock_target = { .file = &clock_file };
	const union uk_fs_create_target generation_target = { .file = &generation_file };
	__u8 snapshot[CLOCK_BYTES];
	__u32 magic, size;
	__u16 version;
	__u64 flags;
	const void *created;
	int rc;

	rc = discover_restore_devices();
	if (rc)
		return rc;
	rc = read_clock(snapshot);
	if (rc)
		return rc;
	memcpy(&magic, snapshot, sizeof(magic));
	memcpy(&size, snapshot + 4, sizeof(size));
	memcpy(&version, snapshot + 8, sizeof(version));
	memcpy(&flags, snapshot + 24, sizeof(flags));
	if (magic != CLOCK_MAGIC || size < CLOCK_BYTES || size > 4096 ||
	    version != 1 || (flags & CLOCK_FLAGS) != CLOCK_FLAGS)
		return -ENODEV;
	memcpy(&read_sequence, snapshot + 12, sizeof(read_sequence));
	rc = uk_random_set_generation_source((volatile const __u8 *)genid_addr);
	if (rc)
		return rc;
	read_genid(witnessed_id);
	read_genid(transport_id);
	uk_waitq_init(&transport_wait);
	created = uk_fs_createat(uk_fs_devfs_root, "vmclock0", 8, 0600,
				O_EXCL, clock_target);
	if (PTRISERR(created))
		return PTR2ERR(created);
	created = uk_fs_createat(uk_fs_devfs_root, "vmgenid", 7, 0600,
				O_EXCL, generation_target);
	if (PTRISERR(created))
		return PTR2ERR(created);
	rc = uk_intctlr_irq_register(irq, restore_irq, NULL);
	return rc;
}

uk_rootfs_initcall_prio(restore_devices_init, 0, UK_FS_PRIO_FSAVAIL);
