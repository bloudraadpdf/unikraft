/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <uk/alloc.h>
#include <uk/blkdev.h>
#include <uk/blkfile.h>
#include <uk/devfs.h>
#include <uk/file/nops.h>
#include <uk/fs.h>
#include <uk/fs/prio.h>
#include <uk/init.h>
#include <uk/mutex.h>
#include <uk/sched.h>

struct block_file {
	struct uk_file file;
	uk_file_refcnt reference;
	struct uk_file_state state;
	struct uk_mutex lock;
	struct uk_blkdev *device;
	char *buffer;
	size_t size, chunk;
	unsigned int id;
	bool readonly;
};

static struct block_file *devices[26];

static ssize_t block_io(const struct uk_file *file, const struct iovec *iov,
			 size_t count, size_t offset, bool write)
{
	struct block_file *block = (struct block_file *)file->node;
	size_t copied = 0;
	int result = 0;

	if (write && block->readonly)
		return -EROFS;
	uk_mutex_lock(&block->lock);
	for (size_t i = 0; i < count; ++i) {
		char *bytes = iov[i].iov_base;
		size_t remaining = iov[i].iov_len;

		while (remaining) {
			size_t aligned, prefix, length, sectors;

			if (offset >= block->size) {
				result = write ? -ENOSPC : 0;
				goto out;
			}
			aligned = offset & ~(size_t)511;
			prefix = offset - aligned;
			length = MIN(remaining, block->chunk - prefix);
			length = MIN(length, block->size - offset);
			if (length > SSIZE_MAX - copied) {
				result = -EOVERFLOW;
				goto out;
			}
			sectors = (prefix + length + 511) / 512;
			if (!write || prefix || length % 512) {
				result = uk_blkdev_sync_io(block->device, 0,
					UK_BLKREQ_READ, aligned / 512, sectors,
					block->buffer);
				if (result)
					goto out;
			}
			if (write) {
				memcpy(block->buffer + prefix, bytes, length);
				result = uk_blkdev_sync_io(block->device, 0,
					UK_BLKREQ_WRITE, aligned / 512, sectors,
					block->buffer);
				if (result)
					goto out;
			} else {
				memcpy(bytes, block->buffer + prefix, length);
			}
			copied += length;
			offset += length;
			bytes += length;
			remaining -= length;
		}
	}
out:
	uk_mutex_unlock(&block->lock);
	return copied ? (ssize_t)copied : result;
}

static ssize_t block_read(const struct uk_file *file, const struct iovec *iov,
			  size_t count, size_t offset, long flags __unused)
{
	return block_io(file, iov, count, offset, false);
}

static struct uk_file_io_result
block_write(const struct uk_file *file, const struct iovec *iov, size_t count,
	    size_t offset, long flags __unused,
	    enum uk_file_write_kind kind __unused)
{
	return uk_file_io_status(block_io(file, iov, count, offset, true));
}

static int block_stat(const struct uk_file *file, unsigned int mask __unused,
		      struct uk_statx *stat)
{
	struct block_file *block = (struct block_file *)file->node;

	memset(stat, 0, sizeof(*stat));
	stat->stx_mask = UK_STATX_BASIC_STATS;
	stat->stx_mode = S_IFBLK | 0600;
	stat->stx_nlink = 1;
	stat->stx_ino = block->id + 1;
	stat->stx_size = block->size;
	stat->stx_blocks = block->size / 512;
	stat->stx_blksize = 512;
	stat->stx_rdev_major = 252;
	stat->stx_rdev_minor = block->id;
	return 0;
}

static int block_control(const struct uk_file *file, int family, int request,
			 uintptr_t arg1, uintptr_t arg2 __unused,
			 uintptr_t arg3 __unused)
{
	struct block_file *block = (struct block_file *)file->node;
	int result;

	if (family == UKFILE_CTL_FILE &&
	    request == UKFILE_CTL_FILE_SYNC_RANGE) {
		uk_mutex_lock(&block->lock);
		uk_mutex_unlock(&block->lock);
		return 0;
	}
	if (family == UKFILE_CTL_FILE && request == UKFILE_CTL_FILE_SYNC) {
		if (block->readonly)
			return 0;
		uk_mutex_lock(&block->lock);
		result = uk_blkdev_capabilities(block->device)->flush_supported ?
			uk_blkdev_sync_io(block->device, 0, UK_BLKREQ_FFLUSH,
					 0, 0, NULL) : 0;
		uk_mutex_unlock(&block->lock);
		return result;
	}
	if (family != UKFILE_CTL_IOCTL)
		return -EOPNOTSUPP;
	if ((unsigned int)request == 0x80081272) {
		if (!arg1)
			return -EFAULT;
		*(__u64 *)arg1 = block->size;
		return 0;
	}
	if (request == 0x1268) {
		if (!arg1)
			return -EFAULT;
		*(int *)arg1 = 512;
		return 0;
	}
	return -ENOTTY;
}

static const struct uk_file_ops block_ops = {
	.read = block_read, .write = block_write,
	.mem = uk_file_nop_mem, .getstat = block_stat,
	.setstat = uk_file_nop_setstat, .ctl = block_control,
};

static void block_completed(struct uk_blkdev *device, uint16_t queue,
			    void *data __unused)
{
	int result = uk_blkdev_queue_finish_reqs(device, queue);

	if (result < 0)
		uk_pr_err("Block completion failed: %d\n", result);
}

static int block_files_init(struct uk_init_ctx *context __unused)
{
	const struct uk_blkdev_conf configuration = { .nb_queues = 1 };
	struct uk_blkdev_queue_conf queue = {
		.a = uk_alloc_get_default(), .callback = block_completed,
		.s = uk_sched_current(),
	};
	unsigned int count = uk_blkdev_count();
	int result;

	if (count > ARRAY_SIZE(devices))
		return -E2BIG;
	for (unsigned int id = 0; id < count; ++id) {
		struct block_file *block = calloc(1, sizeof(*block));
		const struct uk_blkdev_cap *capacity;
		struct uk_blkdev_queue_info info;
		const void *created;
		char name[] = "vda";

		if (!block)
			return -ENOMEM;
		block->device = uk_blkdev_get(id);
		result = uk_blkdev_configure(block->device, &configuration);
		if (!result)
			result = uk_blkdev_queue_get_info(block->device, 0, &info);
		if (!result)
			result = uk_blkdev_queue_configure(block->device, 0,
							info.nb_max, &queue);
		if (!result)
			result = uk_blkdev_start(block->device);
		if (!result)
			result = uk_blkdev_queue_intr_enable(block->device, 0);
		if (result < 0)
			return result;
		capacity = uk_blkdev_capabilities(block->device);
		if (capacity->ssize != 512 || !capacity->max_sectors_per_req ||
		    capacity->sectors > SIZE_MAX / 512)
			return -EOPNOTSUPP;
		block->size = capacity->sectors * 512;
		block->chunk = MIN(capacity->max_sectors_per_req,
				   capacity->mode == O_RDONLY ? 128U : 8U) *
			       512;
		result =
		    posix_memalign((void **)&block->buffer,
				   MAX(capacity->ioalign, 4096), block->chunk);
		if (result)
			return -result;
		block->id = id;
		block->readonly = capacity->mode == O_RDONLY;
		uk_mutex_init(&block->lock);
		block->reference = UK_FILE_REFCNT_INIT_VALUE(block->reference);
		block->state = UK_FILE_STATE_EVENTS_INIT_VALUE(block->state,
							     POLLIN | POLLOUT);
		block->file = (struct uk_file) {
			.vol = devices, .node = block, .ops = &block_ops,
			.refcnt = &block->reference, .state = &block->state,
			._release = uk_file_static_release,
		};
		name[2] += id;
		created = uk_fs_createat(uk_fs_devfs_root, name, 3, 0600,
				O_EXCL, (union uk_fs_create_target) {
					.file = &block->file,
				});
		if (PTRISERR(created))
			return PTR2ERR(created);
		devices[id] = block;
	}
	return 0;
}

uk_rootfs_initcall_prio(block_files_init, 0, UK_FS_PRIO_FSAVAIL);

const struct uk_file *uk_blkfile_open(const char *name)
{
	unsigned int id;

	if (!name || strncmp(name, "/dev/vd", 7) ||
	    !name[7] || name[8] || name[7] < 'a' || name[7] > 'z')
		return ERR2PTR(-ENODEV);
	id = name[7] - 'a';
	return devices[id] ? &devices[id]->file : ERR2PTR(-ENODEV);
}
