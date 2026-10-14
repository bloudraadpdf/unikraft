/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <ext4_bcache.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <uk/blkfile.h>
#include <uk/file.h>
#include <uk/fs.h>
#include <uk/fs/dirent.h>
#include <uk/fs/driver.h>
#include <uk/fs/template/live.h>
#include <uk/mutex.h>
#include <uk/posix-time.h>
#include <uk/syscall.h>
#include "file-pages.h"

struct disk_node;
struct disk_volume {
	struct disk_volume *next;
	int claimed, closing;
	unsigned int users;
	struct ext4_fs fs;
	struct ext4_blockdev device;
	struct ext4_blockdev_iface interface;
	struct ext4_bcache cache;
	struct jbd_fs journal_fs;
	struct jbd_journal journal;
	struct uk_mutex lock;
	const struct uk_file *source;
	struct disk_node *nodes;
	unsigned char sector[512];
	int error;
};
struct disk_node {
	struct disk_volume *volume;
	struct disk_node *next;
	struct uk_file_state state;
	struct ukext4_pages pages;
	uint32_t inode;
	size_t size;
	unsigned int type, references;
	char *link;
	size_t link_length;
};
UK_FS_TMPL_LIVE_TYPES(ext4, struct disk_node *);
UK_FS_TMPL_LIVE_OPS(ext4, struct disk_node *);

static struct uk_mutex mounted_lock = UK_MUTEX_INITIALIZER(mounted_lock);
static struct disk_volume *mounted_volumes;

static int volume_finish(struct disk_volume *volume);
static int inode_delete(struct ext4_inode_ref *inode);
static void inode_touch(struct ext4_inode_ref *inode, int modify);

static int volume_claim(struct disk_volume *volume)
{
	uk_mutex_lock(&mounted_lock);
	struct disk_volume **entry = &mounted_volumes;
	while (*entry) {
		struct disk_volume *current = *entry;
		if (current->source != volume->source) {
			entry = &current->next;
			continue;
		}
		uk_mutex_lock(&current->lock);
		if (current->users || current->closing) {
			uk_mutex_unlock(&current->lock);
			uk_mutex_unlock(&mounted_lock);
			return EBUSY;
		}
		current->closing = 1;
		uk_mutex_unlock(&current->lock);
		int result = volume_finish(current);
		if (result) {
			current->closing = 0;
			uk_mutex_unlock(&mounted_lock);
			return result;
		}
		*entry = current->next;
		uk_file_release(current->source);
		free(current);
		break;
	}
	volume->next = mounted_volumes;
	mounted_volumes = volume;
	volume->claimed = 1;
	uk_mutex_unlock(&mounted_lock);
	return 0;
}
static void volume_unclaim(struct disk_volume *volume)
{
	struct disk_volume **entry;

	if (!volume->claimed)
		return;
	uk_mutex_lock(&mounted_lock);
	for (entry = &mounted_volumes; *entry != volume; entry = &(*entry)->next)
		;
	*entry = volume->next;
	uk_mutex_unlock(&mounted_lock);
}

static int disk_open(struct ext4_blockdev *device __unused)
{
	return 0;
}
static int disk_transfer(struct ext4_blockdev *device, void *buffer,
		uint64_t first, uint32_t blocks, int write)
{
	struct disk_volume *volume = device->bdif->p_user;
	const size_t length = (size_t)blocks * 512;
	struct iovec vector = { .iov_base = buffer, .iov_len = length };
	ssize_t result;

	if (first > device->part_size / 512 ||
	    blocks > device->part_size / 512 - first)
		return EIO;
	result = write ? uk_file_write(volume->source, &vector, 1, first * 512, 0) :
		uk_file_read(volume->source, &vector, 1, first * 512, 0);
	return result < 0 ? -result : (size_t)result == length ? 0 : EIO;
}
static int disk_read(struct ext4_blockdev *device, void *buffer,
		uint64_t first, uint32_t blocks)
{
	return disk_transfer(device, buffer, first, blocks, 0);
}
static int disk_write(struct ext4_blockdev *device, const void *buffer,
		uint64_t first, uint32_t blocks)
{
	return disk_transfer(device, (void *)buffer, first, blocks, 1);
}
static int disk_flush(struct ext4_blockdev *device)
{
	struct disk_volume *volume = device->bdif->p_user;

	return -uk_file_ctl(volume->source, UKFILE_CTL_FILE,
			    UKFILE_CTL_FILE_SYNC, 0, 0, 0);
}
static int transaction_begin(struct disk_volume *volume)
{
	if (volume->error)
		return volume->error;
	if (!volume->fs.jbd_journal)
		return 0;
	UK_ASSERT(!volume->fs.curr_trans);
	volume->fs.curr_trans = jbd_journal_new_trans(&volume->journal);
	return volume->fs.curr_trans ? 0 : ENOMEM;
}
static int transaction_end(struct disk_volume *volume, int result)
{
	if (volume->fs.curr_trans) {
		int committed = jbd_journal_commit_trans(&volume->journal,
							volume->fs.curr_trans);
		volume->fs.curr_trans = NULL;
		if (committed) {
			volume->error = committed;
			if (!result)
				result = committed;
		}
	}
	return result;
}
static int inode_put(struct ext4_inode_ref *inode, int result)
{
	int released = ext4_fs_put_inode_ref(inode);

	return result ? result : released;
}
static int node_update_begin(struct disk_node *node, struct ext4_inode_ref *ref)
{
	struct disk_volume *volume = node->volume;
	int result;

	uk_mutex_lock(&volume->lock);
	result = transaction_begin(volume);
	if (!result)
		result = ext4_fs_get_inode_ref(&volume->fs, node->inode, ref);
	if (result) {
		result = transaction_end(volume, result);
		uk_mutex_unlock(&volume->lock);
	}
	return result;
}
static int node_update_end(struct disk_node *node, struct ext4_inode_ref *ref,
		int result)
{
	result = inode_put(ref, result);
	result = transaction_end(node->volume, result);
	uk_mutex_unlock(&node->volume->lock);
	return result;
}
static int node_writeback(struct disk_node *node)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref inode;
	int changed = 0, deleting = 0;
	int result;

	if ((!node->pages.head && node->references) || volume->fs.read_only)
		return 0;
	result = transaction_begin(volume);
	if (!result) {
		result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &inode);
		if (!result) {
			if (!node->references && !ext4_inode_get_links_cnt(inode.inode)) {
				deleting = 1;
				result = inode_delete(&inode);
			}
			else
				result = ukext4_pages_writeback(&node->pages, &inode, &changed);
			if (changed)
				inode_touch(&inode, 1);
			result = inode_put(&inode, result);
		}
	}
	result = transaction_end(volume, result);
	if (result && deleting)
		volume->error = result;
	return result;
}
static void node_free(struct disk_node *node)
{
	ukext4_pages_clear(&node->pages);
	free(node->link);
	free(node);
}
static int sync_locked(struct disk_volume *volume)
{
	int result = volume->error;

	if (result || volume->fs.read_only)
		return result;
	struct disk_node **entry = &volume->nodes;
	while (*entry) {
		struct disk_node *node = *entry;
		result = node_writeback(node);
		if (result)
			return result;
		if (node->references)
			entry = &node->next;
		else {
			*entry = node->next;
			node_free(node);
		}
	}
	if (volume->fs.jbd_journal)
		jbd_journal_purge_cp_trans(&volume->journal, true, false);
	result = ext4_block_cache_flush(&volume->device);
	if (!result)
		result = ext4_sb_write(&volume->device, &volume->fs.sb);
	if (!result)
		result = disk_flush(&volume->device);
	return result;
}
static int volume_sync(struct disk_node *node)
{
	int result;

	uk_mutex_lock(&node->volume->lock);
	result = sync_locked(node->volume);
	uk_mutex_unlock(&node->volume->lock);
	return -result;
}
static int volume_finish(struct disk_volume *volume)
{
	int result = sync_locked(volume);
	if (result)
		return result;

	if (volume->fs.jbd_journal) {
		int stopped = jbd_journal_stop(&volume->journal);
		int released = jbd_put_fs(&volume->journal_fs);
		if (!result)
			result = stopped ? stopped : released;
	}
	if (!result)
		result = ext4_fs_fini(&volume->fs);
	if (!result)
		result = disk_flush(&volume->device);
	if (result) {
		volume->error = result;
		return result;
	}
	ext4_bcache_cleanup(&volume->cache);
	ext4_bcache_fini_dynamic(&volume->cache);
	ext4_block_fini(&volume->device);
	return 0;
}
static void volume_close(struct disk_volume *volume)
{
	int result = volume_finish(volume);
	if (result) {
		uk_pr_err("ext4 volume close: %d\n", result);
		uk_mutex_lock(&volume->lock);
		volume->closing = 0;
		uk_mutex_unlock(&volume->lock);
		return;
	}
	volume_unclaim(volume);
	uk_file_release(volume->source);
	free(volume);
}
static int inode_delete(struct ext4_inode_ref *inode)
{
	int result;

	if (ext4_inode_has_flag(inode->inode, EXT4_INODE_FLAG_EXTENTS)) {
		result = ext4_extent_remove_space(inode, 0, EXT_MAX_BLOCKS);
		if (result)
			return result;
		ext4_inode_set_size(inode->inode, 0);
		inode->dirty = true;
	} else {
		result = ext4_fs_truncate_inode(inode, 0);
		if (result)
			return result;
	}
	ext4_inode_set_del_time(inode->inode, UINT32_MAX);
	return ext4_fs_free_inode(inode);
}
static void node_acquire_locked(struct disk_node *node)
{
	node->references++;
	node->volume->users++;
}
static void node_acquire(struct disk_node *node)
{
	uk_mutex_lock(&node->volume->lock);
	node_acquire_locked(node);
	uk_mutex_unlock(&node->volume->lock);
}
static void node_release(struct disk_node *node)
{
	struct disk_volume *volume = node->volume;
	struct disk_node **entry;
	int result;

	uk_mutex_lock(&volume->lock);
	volume->users--;
	if (--node->references) {
		uk_mutex_unlock(&volume->lock);
		return;
	}
	for (entry = &volume->nodes; *entry != node; entry = &(*entry)->next)
		;
	result = node_writeback(node);
	if (!result) {
		*entry = node->next;
		node_free(node);
	}
	int empty = !volume->users;
	if (empty)
		volume->closing = 1;
	uk_mutex_unlock(&volume->lock);
	if (empty)
		volume_close(volume);
}
static struct disk_node *node_get(struct disk_volume *volume, uint32_t number)
{
	struct disk_node *node;
	struct ext4_inode_ref inode;
	int result;

	for (node = volume->nodes; node; node = node->next)
		if (node->inode == number) {
			node_acquire_locked(node);
			return node;
		}
	result = ext4_fs_get_inode_ref(&volume->fs, number, &inode);
	if (result)
		return ERR2PTR(-result);
	node = calloc(1, sizeof(*node));
	if (!node) {
		ext4_fs_put_inode_ref(&inode);
		return ERR2PTR(-ENOMEM);
	}
	node->volume = volume;
	node->inode = number;
	node->type = ext4_inode_type(&volume->fs.sb, inode.inode);
	node->size = ext4_inode_get_size(&volume->fs.sb, inode.inode);
	node->references = 1;
	node->state = (struct uk_file_state)UK_FILE_STATE_INITIALIZER(node->state);
	if (node->type == S_IFLNK) {
		uint64_t length = ext4_inode_get_size(&volume->fs.sb, inode.inode);
		if (length > PATH_MAX) {
			result = ENAMETOOLONG;
			goto out;
		}
		node->link_length = length;
		node->link = malloc(length + 1);
		if (!node->link) {
			result = ENOMEM;
			goto out;
		}
		if (length < sizeof(inode.inode->blocks) &&
		    !ext4_inode_get_blocks_count(&volume->fs.sb, inode.inode)) {
			memcpy(node->link, inode.inode->blocks, length);
		} else {
			size_t count;
			result = ukext4_inode_read(&inode, node->link, length, 0, &count);
			if (!result && count != length)
				result = EIO;
		}
		node->link[length] = 0;
	}
out:
	result = inode_put(&inode, result);
	if (result) {
		free(node->link);
		free(node);
		return ERR2PTR(-result);
	}
	uk_file_state_event_set(&node->state, POLLIN | POLLOUT);
	volume->users++;
	node->next = volume->nodes;
	volume->nodes = node;
	return node;
}
static struct disk_node *volume_open(union uk_fs_vopen_vol input,
		unsigned long flags, union uk_fs_vopen_data data __unused,
		size_t format)
{
	struct disk_volume *volume;
	struct disk_node *root;
	struct uk_statx stat;
	int result, initialised = 0, cached = 0;

	if ((format & UK_FS_VOPEN_VOL_MASK) != UK_FS_VOPEN_VOL_RAW)
		return ERR2PTR(-EINVAL);
	volume = calloc(1, sizeof(*volume));
	if (!volume)
		return ERR2PTR(-ENOMEM);
	uk_mutex_init(&volume->lock);
	volume->source = uk_blkfile_open(input.raw);
	if (PTRISERR(volume->source)) {
		result = -PTR2ERR(volume->source);
		volume->source = NULL;
		goto fail;
	}
	uk_file_acquire(volume->source);
	result = volume_claim(volume);
	if (result)
		goto fail;
	result = -uk_file_getstat(volume->source, UK_STATX_SIZE, &stat);
	if (result)
		goto fail;
	volume->interface = (struct ext4_blockdev_iface) {
		.open = disk_open, .bread = disk_read, .bwrite = disk_write,
		.close = disk_flush, .ph_bsize = 512, .ph_bcnt = stat.stx_size / 512,
		.ph_bbuf = volume->sector, .p_user = volume
	};
	volume->device.bdif = &volume->interface;
	volume->device.part_size = stat.stx_size;
	result = ext4_block_init(&volume->device);
	if (result)
		goto fail;
	initialised = 1;
	result = ext4_fs_init(&volume->fs, &volume->device, !!(flags & MS_RDONLY));
	if (result)
		goto fail;
	if (!(flags & MS_RDONLY) && volume->fs.read_only) {
		result = EROFS;
		goto fail;
	}
	uint32_t size = ext4_sb_get_block_size(&volume->fs.sb);
	ext4_block_set_lb_size(&volume->device, size);
	result = ext4_bcache_init_dynamic(&volume->cache, CONFIG_BLOCK_DEV_CACHE_SIZE, size);
	if (result)
		goto fail;
	cached = 1;
	result = ext4_block_bind_bcache(&volume->device, &volume->cache);
	if (result)
		goto fail;
	volume->device.fs = &volume->fs;
	if (ext4_sb_feature_com(&volume->fs.sb, EXT4_FCOM_HAS_JOURNAL)) {
		result = jbd_get_fs(&volume->fs, &volume->journal_fs);
		if (result)
			goto fail;
		if (ext4_sb_feature_incom(&volume->fs.sb, EXT4_FINCOM_RECOVER))
			result = jbd_recover(&volume->journal_fs);
		if (!result && !volume->fs.read_only)
			result = jbd_journal_start(&volume->journal_fs, &volume->journal);
		if (result || volume->fs.read_only) {
			jbd_put_fs(&volume->journal_fs);
			if (result)
				goto fail;
		} else {
			volume->fs.jbd_fs = &volume->journal_fs;
			volume->fs.jbd_journal = &volume->journal;
		}
	}
	root = node_get(volume, EXT4_INODE_ROOT_INDEX);
	if (PTRISERR(root)) {
		result = -PTR2ERR(root);
		goto fail;
	}
	if (root->type != S_IFDIR) {
		node_release(root);
		return ERR2PTR(-ENOTDIR);
	}
	return root;
fail:
	if (volume->fs.jbd_journal) {
		jbd_journal_stop(&volume->journal);
		jbd_put_fs(&volume->journal_fs);
	}
	if (cached) {
		ext4_bcache_cleanup(&volume->cache);
		ext4_bcache_fini_dynamic(&volume->cache);
	}
	if (initialised)
		ext4_block_fini(&volume->device);
	volume_unclaim(volume);
	if (volume->source)
		uk_file_release(volume->source);
	free(volume);
	return ERR2PTR(-result);
}
static void inode_touch(struct ext4_inode_ref *inode, int modify)
{
	struct timespec now;

	if (uk_sys_clock_gettime(CLOCK_REALTIME, &now))
		return;
	ext4_inode_set_change_inode_time(inode->inode, now.tv_sec);
	if (modify)
		ext4_inode_set_modif_time(inode->inode, now.tv_sec);
	if (ext4_get16(&inode->fs->sb, inode_size) >= sizeof(struct ext4_inode) &&
	    ext4_inode_get_extra_isize(&inode->fs->sb, inode->inode) >= 16) {
		ext4_set32(inode->inode, ctime_extra,
			   ((uint32_t)now.tv_nsec << 2) | ((uint64_t)now.tv_sec >> 32));
		if (modify)
			ext4_set32(inode->inode, mtime_extra,
				   ((uint32_t)now.tv_nsec << 2) | ((uint64_t)now.tv_sec >> 32));
	}
	inode->dirty = true;
}
static ssize_t node_io(struct disk_node *node, const struct iovec *iov,
		size_t count, size_t offset, unsigned long flags, int write)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref inode;
	size_t total = 0;
	int result;

	if (node->type != S_IFREG)
		return node->type == S_IFDIR ? -EISDIR : -EINVAL;
	if (write && ((flags & MS_RDONLY) || volume->fs.read_only))
		return -EROFS;
	uk_mutex_lock(&volume->lock);
	result = write ? transaction_begin(volume) : volume->error;
	if (result)
		goto out;
	result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &inode);
	if (result)
		goto end;
	for (size_t i = 0; i < count; i++) {
		size_t done = 0;
		result = ukext4_pages_io(&node->pages, &inode, iov[i].iov_base,
					  iov[i].iov_len, offset, &done, write);
		total += done;
		offset += done;
		if (result || done < iov[i].iov_len)
			break;
	}
	node->size = ext4_inode_get_size(&volume->fs.sb, inode.inode);
	if (write && total)
		inode_touch(&inode, 1);
	result = inode_put(&inode, result);
end:
	if (write)
		result = transaction_end(volume, result);
out:
	uk_mutex_unlock(&volume->lock);
	return total ? (ssize_t)total : -result;
}
static ssize_t node_read(struct disk_node *node, const struct iovec *iov,
		size_t count, size_t offset, long flags __unused, unsigned long mount)
{
	return node_io(node, iov, count, offset, mount, 0);
}
static ssize_t node_write(struct disk_node *node, const struct iovec *iov,
		size_t count, size_t offset, long flags __unused, unsigned long mount)
{
	return node_io(node, iov, count, offset, mount, 1);
}
static ssize_t node_mem(struct disk_node *node, enum uk_file_mem_op op,
		size_t offset, size_t length, struct iovec *iov,
		size_t count, unsigned long flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref inode;
	ssize_t result;

	if (node->type != S_IFREG)
		return -ENODEV;
	if (op == UKFILE_MEM_RETRIEVE && ((flags & MS_RDONLY) || volume->fs.read_only))
		return -EROFS;
	uk_mutex_lock(&volume->lock);
	if (op == UKFILE_MEM_RELEASE) {
		ukext4_pages_release(&node->pages, offset, length, node->size);
		uk_mutex_unlock(&volume->lock);
		return 0;
	}
	result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &inode);
	if (!result) {
		result = ukext4_pages_mem(&node->pages, &inode, op, offset, length, iov, count);
		int released = inode_put(&inode, 0);
		if (released && result >= 0) {
			if (op == UKFILE_MEM_ACQUIRE && result)
				ukext4_pages_release(&node->pages, offset, result, node->size);
			result = -released;
		}
	} else {
		result = -result;
	}
	uk_mutex_unlock(&volume->lock);
	return result;
}
static int node_stat(struct disk_node *node, unsigned int mask __unused,
		struct uk_statx *stat, unsigned long flags __unused)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref ref;
	int result;

	uk_mutex_lock(&volume->lock);
	result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &ref);
	if (!result) {
		struct ext4_inode *inode = ref.inode;
		memset(stat, 0, sizeof(*stat));
		stat->stx_mask = UK_STATX_BASIC_STATS;
		stat->stx_blksize = ext4_sb_get_block_size(&volume->fs.sb);
		stat->stx_mode = ext4_inode_get_mode(&volume->fs.sb, inode);
		stat->stx_uid = ext4_inode_get_uid(inode);
		stat->stx_gid = ext4_inode_get_gid(inode);
		stat->stx_nlink = ext4_inode_get_links_cnt(inode);
		stat->stx_ino = node->inode;
		stat->stx_size = ext4_inode_get_size(&volume->fs.sb, inode);
		stat->stx_blocks = ext4_inode_get_blocks_count(&volume->fs.sb, inode);
		stat->stx_atime.tv_sec = (int32_t)ext4_inode_get_access_time(inode);
		stat->stx_mtime.tv_sec = (int32_t)ext4_inode_get_modif_time(inode);
		stat->stx_ctime.tv_sec = (int32_t)ext4_inode_get_change_inode_time(inode);
		if (ext4_get16(&volume->fs.sb, inode_size) >= sizeof(*inode) &&
		    ext4_inode_get_extra_isize(&volume->fs.sb, inode) >= 16) {
			uint32_t atime = ext4_get32(inode, atime_extra);
			uint32_t mtime = ext4_get32(inode, mtime_extra);
			uint32_t ctime = ext4_get32(inode, ctime_extra);
			stat->stx_atime.tv_sec += (int64_t)(atime & 3) << 32;
			stat->stx_mtime.tv_sec += (int64_t)(mtime & 3) << 32;
			stat->stx_ctime.tv_sec += (int64_t)(ctime & 3) << 32;
			stat->stx_atime.tv_nsec = atime >> 2;
			stat->stx_mtime.tv_nsec = mtime >> 2;
			stat->stx_ctime.tv_nsec = ctime >> 2;
		}
		result = inode_put(&ref, 0);
	}
	uk_mutex_unlock(&volume->lock);
	return -result;
}
static int node_setstat(struct disk_node *node, unsigned int mask,
		const struct uk_statx *stat, unsigned long flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref ref;
	int result;

	if ((flags & MS_RDONLY) || volume->fs.read_only)
		return -EROFS;
	if (mask & ~(UK_STATX_MODE | UK_STATX_UID | UK_STATX_GID |
		     UK_STATX_ATIME | UK_STATX_MTIME | UK_STATX_CTIME))
		return -EINVAL;
	result = node_update_begin(node, &ref);
	if (result)
		return -result;
	if (mask & UK_STATX_MODE)
		ext4_inode_set_mode(&volume->fs.sb, ref.inode,
				    node->type | (stat->stx_mode & 07777));
	if (mask & UK_STATX_UID)
		ext4_inode_set_uid(ref.inode, stat->stx_uid);
	if (mask & UK_STATX_GID)
		ext4_inode_set_gid(ref.inode, stat->stx_gid);
	if (mask & UK_STATX_ATIME)
		ext4_inode_set_access_time(ref.inode, stat->stx_atime.tv_sec);
	if (mask & UK_STATX_MTIME)
		ext4_inode_set_modif_time(ref.inode, stat->stx_mtime.tv_sec);
	inode_touch(&ref, 0);
	if (ext4_get16(&volume->fs.sb, inode_size) >= sizeof(*ref.inode) &&
	    ext4_inode_get_extra_isize(&volume->fs.sb, ref.inode) >= 16) {
		if (mask & UK_STATX_ATIME)
			ext4_set32(ref.inode, atime_extra,
				   (stat->stx_atime.tv_nsec << 2) |
				   ((uint64_t)stat->stx_atime.tv_sec >> 32));
		if (mask & UK_STATX_MTIME)
			ext4_set32(ref.inode, mtime_extra,
				   (stat->stx_mtime.tv_nsec << 2) |
				   ((uint64_t)stat->stx_mtime.tv_sec >> 32));
	}
	return -node_update_end(node, &ref, result);
}
static int node_control(struct disk_node *node, int family, int request,
		uintptr_t arg1, uintptr_t arg2 __unused, uintptr_t arg3 __unused,
		unsigned long flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref ref;
	int result;

	if (family != UKFILE_CTL_FILE)
		return family == UKFILE_CTL_IOCTL ? -ENOTTY : -EOPNOTSUPP;
	if (request == UKFILE_CTL_FILE_SYNC)
		return volume_sync(node);
	if (request == UKFILE_CTL_FILE_FADVISE)
		return 0;
	if (request != UKFILE_CTL_FILE_TRUNC)
		return -EOPNOTSUPP;
	if ((flags & MS_RDONLY) || volume->fs.read_only)
		return -EROFS;
	if ((off_t)arg1 < 0 || node->type != S_IFREG)
		return -EINVAL;
	result = node_update_begin(node, &ref);
	if (result)
		return -result;
	result = ukext4_inode_resize(&ref, arg1);
	if (!result) {
		node->size = arg1;
		ukext4_pages_zero(&node->pages, arg1);
		inode_touch(&ref, 1);
	}
	return -node_update_end(node, &ref, result);
}
static int volume_stat(struct disk_node *node, struct statfs *stat)
{
	struct disk_volume *volume = node->volume;

	uk_mutex_lock(&volume->lock);
	memset(stat, 0, sizeof(*stat));
	stat->f_type = 0xef53;
	stat->f_bsize = ext4_sb_get_block_size(&volume->fs.sb);
	stat->f_frsize = stat->f_bsize;
	stat->f_blocks = ext4_sb_get_blocks_cnt(&volume->fs.sb);
	stat->f_bfree = ext4_sb_get_free_blocks_cnt(&volume->fs.sb);
	uint64_t reserved = ((uint64_t)ext4_get32(&volume->fs.sb, reserved_blocks_count_hi) << 32) |
		ext4_get32(&volume->fs.sb, reserved_blocks_count_lo);
	stat->f_bavail = stat->f_bfree > reserved ? stat->f_bfree - reserved : 0;
	stat->f_files = ext4_get32(&volume->fs.sb, inodes_count);
	stat->f_ffree = ext4_get32(&volume->fs.sb, free_inodes_count);
	stat->f_namelen = EXT4_DIRECTORY_FILENAME_LEN;
	uk_mutex_unlock(&volume->lock);
	return 0;
}
static int find_inode(struct ext4_inode_ref *parent, const char *name,
		size_t length, uint32_t *number)
{
	struct ext4_dir_search_result entry;
	int result;

	if (uk_fs_path_isdot(name, length) || uk_fs_path_isdotdot(name, length)) {
		ext4_fsblk_t physical;
		struct ext4_block block;
		struct ext4_dir_en *found;
		result = ext4_fs_get_inode_dblk_idx(parent, 0, &physical, false);
		if (result)
			return result;
		result = ext4_block_get(parent->fs->bdev, &block, physical);
		if (result)
			return result;
		result = ext4_dir_find_in_block(&block, &parent->fs->sb,
						length, name, &found);
		if (!result)
			*number = ext4_dir_en_get_inode(found);
		int released = ext4_block_set(parent->fs->bdev, &block);
		return result ? result : released;
	}
	result = ext4_dir_find_entry(&entry, parent, name, length);

	if (result)
		return result;
	*number = ext4_dir_en_get_inode(entry.dentry);
	return ext4_dir_destroy_result(parent, &entry);
}
static int node_lookup(struct disk_node *node, const char *path, size_t length,
		struct disk_node **out, union uk_fs_lookup_out *details __unused,
		size_t *consumed)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref ref;
	struct disk_node *found = NULL;
	uint32_t number;
	size_t len = uk_fs_path_sep(path, length);
	int result;

	*consumed = 0;
	if (!len || len > EXT4_DIRECTORY_FILENAME_LEN)
		return -ENAMETOOLONG;
	uk_mutex_lock(&volume->lock);
	result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &ref);
	if (!result) {
		result = -uk_fs_checkperm(X_OK,
			ext4_inode_get_mode(&volume->fs.sb, ref.inode) & 0777,
			ext4_inode_get_uid(ref.inode), ext4_inode_get_gid(ref.inode));
		if (!result)
			result = find_inode(&ref, path, len, &number);
		result = inode_put(&ref, result);
	}
	if (!result) {
		found = node_get(volume, number);
		if (PTRISERR(found))
			result = -PTR2ERR(found);
		else if (found->type == S_IFLNK)
			node_acquire_locked(node);
	}
	uk_mutex_unlock(&volume->lock);
	if (result)
		return -result;
	out[0] = found;
	*consumed = len < length ? len + 1 : len;
	if (found->type == S_IFLNK) {
		out[1] = node;
		return UKFS_STOP_SYM;
	}
	return UKFS_STOP_NOD;
}
static ssize_t node_list(struct disk_node *node, size_t *cursor,
		void *buffer, size_t length, unsigned long flags __unused)
{
	static const unsigned char types[] = {
		DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
	};
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref ref;
	struct ext4_dir_iter iterator;
	size_t used = 0;
	int result;

	uk_mutex_lock(&volume->lock);
	result = ext4_fs_get_inode_ref(&volume->fs, node->inode, &ref);
	if (result)
		goto out;
	result = ext4_dir_iterator_init(&iterator, &ref, *cursor);
	if (result)
		goto release;
	while (iterator.curr) {
		struct ext4_dir_en *current = iterator.curr;
		uint32_t number = ext4_dir_en_get_inode(current);
		uint16_t len = ext4_dir_en_get_name_len(&volume->fs.sb, current);
		unsigned int type = ext4_dir_en_get_inode_type(&volume->fs.sb, current);
		size_t record = UKFS_DIRENT_RECLEN(len);
		struct uk_fs_dirent *entry = NULL;

		if (number && record > length - used) {
			if (!used)
				result = EINVAL;
			break;
		}
		if (number) {
			entry = (struct uk_fs_dirent *)((char *)buffer + used);
			memset(entry, 0, record);
			entry->d_ino = number;
			entry->d_reclen = record;
			entry->d_type = type < sizeof(types) ? types[type] : DT_UNKNOWN;
			memcpy(entry->d_name, current->name, len);
			used += record;
		}
		result = ext4_dir_iterator_next(&iterator);
		if (result)
			break;
		*cursor = iterator.curr ? iterator.curr_off :
			ext4_inode_get_size(&volume->fs.sb, ref.inode);
		if (entry)
			entry->d_off = *cursor;
	}
	int released = ext4_dir_iterator_fini(&iterator);
	if (!result)
		result = released;
release:
	result = inode_put(&ref, result);
out:
	uk_mutex_unlock(&volume->lock);
	return used ? (ssize_t)used : -result;
}
static int valid_name(const char *name, size_t length)
{
	if (!length || memchr(name, '/', length) || memchr(name, 0, length))
		return EINVAL;
	if (length > EXT4_DIRECTORY_FILENAME_LEN)
		return ENAMETOOLONG;
	if (uk_fs_path_isdot(name, length) || uk_fs_path_isdotdot(name, length))
		return EINVAL;
	return 0;
}
static int directory_empty(struct ext4_inode_ref *ref)
{
	struct ext4_dir_iter iterator;
	int result = ext4_dir_iterator_init(&iterator, ref, 0);

	if (result)
		return result;
	while (iterator.curr) {
		uint16_t length = ext4_dir_en_get_name_len(&ref->fs->sb, iterator.curr);
		if (ext4_dir_en_get_inode(iterator.curr) &&
		    !uk_fs_path_isdot((const char *)iterator.curr->name, length) &&
		    !uk_fs_path_isdotdot((const char *)iterator.curr->name, length)) {
			result = ENOTEMPTY;
			break;
		}
		result = ext4_dir_iterator_next(&iterator);
		if (result)
			break;
	}
	int released = ext4_dir_iterator_fini(&iterator);
	return result ? result : released;
}
static int drop_link(struct disk_volume *volume, struct ext4_inode_ref *ref,
		struct ext4_inode_ref *parent)
{
	int directory = ext4_inode_type(&volume->fs.sb, ref->inode) == S_IFDIR;
	uint16_t links = ext4_inode_get_links_cnt(ref->inode);

	if (!links)
		return EIO;
	ext4_inode_set_links_cnt(ref->inode, directory ? 0 : links - 1);
	if (directory) {
		ext4_fs_inode_links_count_dec(parent);
		parent->dirty = true;
	}
	inode_touch(ref, 0);
	if (!ext4_inode_get_links_cnt(ref->inode)) {
		for (struct disk_node *node = volume->nodes; node; node = node->next)
			if (node->inode == ref->index)
				return 0;
		return inode_delete(ref);
	}
	return 0;
}
static int initialise_child(struct disk_volume *volume,
		struct ext4_inode_ref *parent, struct ext4_inode_ref *child,
		unsigned int mode, const char *link)
{
	unsigned int type = mode & S_IFMT;
	int result = 0;

	ext4_inode_set_mode(&volume->fs.sb, child->inode, mode);
	ext4_inode_set_uid(child->inode, uk_syscall_r_geteuid());
	uint32_t parent_mode = ext4_inode_get_mode(&volume->fs.sb, parent->inode);
	ext4_inode_set_gid(child->inode, parent_mode & S_ISGID ?
		ext4_inode_get_gid(parent->inode) : (uint32_t)uk_syscall_r_getegid());
	if (type == S_IFDIR && (parent_mode & S_ISGID))
		ext4_inode_set_mode(&volume->fs.sb, child->inode, mode | S_ISGID);
	inode_touch(child, 1);
	ext4_inode_set_access_time(child->inode, ext4_inode_get_modif_time(child->inode));
	if (type == S_IFLNK && strlen(link) < sizeof(child->inode->blocks)) {
		memcpy(child->inode->blocks, link, strlen(link));
		ext4_inode_set_size(child->inode, strlen(link));
		return 0;
	}
	ext4_fs_inode_blocks_init(&volume->fs, child);
	if (type == S_IFDIR) {
		if (ext4_sb_feature_com(&volume->fs.sb, EXT4_FCOM_DIR_INDEX)) {
			result = ext4_dir_dx_init(child, parent);
			if (!result)
				ext4_inode_set_flag(child->inode, EXT4_INODE_FLAG_INDEX);
		} else {
			result = ext4_dir_add_entry(child, ".", 1, child);
			if (!result)
				result = ext4_dir_add_entry(child, "..", 2, parent);
		}
	} else if (type == S_IFLNK) {
		size_t count;
		result = ukext4_inode_write(child, link, strlen(link), 0, &count);
		if (!result && count != strlen(link))
			result = EIO;
	}
	return result;
}
static struct disk_node *node_create(struct disk_node *node,
		const char *name, size_t length, unsigned int mode, int flags,
		union UK_FS_TMPL_LIVE_CREATE_TARGET(ext4) target,
		unsigned long mount_flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref parent, child;
	struct disk_node *created = NULL;
	unsigned int type = mode & S_IFMT;
	int temporary = (flags & O_TMPFILE) == O_TMPFILE;
	int hardlink = type == S_IFMT;
	int allocated = 0, linked = 0;
	uint32_t number;
	int result;

	if ((mount_flags & MS_RDONLY) || volume->fs.read_only)
		return ERR2PTR(-EROFS);
	if (!temporary && (result = valid_name(name, length)))
		return ERR2PTR(-result);
	if (type != S_IFREG && type != S_IFDIR && type != S_IFLNK && !hardlink)
		return ERR2PTR(-EOPNOTSUPP);
	if (hardlink && target.livenode->volume != volume)
		return ERR2PTR(-EXDEV);
	if (hardlink && target.livenode->type == S_IFDIR)
		return ERR2PTR(-EPERM);
	if (temporary && type != S_IFREG)
		return ERR2PTR(-EINVAL);
	if (type == S_IFLNK && strlen(target.ukfs.path) > PATH_MAX)
		return ERR2PTR(-ENAMETOOLONG);
	result = node_update_begin(node, &parent);
	if (result)
		return ERR2PTR(-result);
	if (!ext4_inode_get_links_cnt(parent.inode)) {
		result = ENOENT;
		goto put_parent;
	}
	if (!temporary) {
		result = find_inode(&parent, name, length, &number);
		if (!result) {
			if ((flags & O_EXCL) || hardlink || type != S_IFREG)
				result = EEXIST;
			else
				created = node_get(volume, number);
			goto put_parent;
		}
		if (result != ENOENT)
			goto put_parent;
	}
	if (hardlink) {
		result = ext4_fs_get_inode_ref(&volume->fs, target.livenode->inode, &child);
	} else {
		int filetype = type == S_IFDIR ? EXT4_DE_DIR :
			       type == S_IFLNK ? EXT4_DE_SYMLINK : EXT4_DE_REG_FILE;
		result = ext4_fs_alloc_inode(&volume->fs, &child, filetype);
		allocated = !result;
	}
	if (result)
		goto put_parent;
	if (allocated)
		result = initialise_child(volume, &parent, &child, mode,
					  type == S_IFLNK ? target.ukfs.path : NULL);
	if (!result && ext4_inode_get_links_cnt(child.inode) == UINT16_MAX)
		result = EMLINK;
	if (!result && !temporary) {
		result = ext4_dir_add_entry(&parent, name, length, &child);
		if (!result) {
			linked = 1;
			ext4_fs_inode_links_count_inc(&child);
			if (type == S_IFDIR) {
				ext4_fs_inode_links_count_inc(&child);
				ext4_fs_inode_links_count_inc(&parent);
			}
			inode_touch(&parent, 1);
			inode_touch(&child, 0);
		}
	}
	if (!result)
		created = node_get(volume, child.index);
	if (created && PTRISERR(created))
		result = -PTR2ERR(created);
	if (result && allocated && !linked) {
		int removed = inode_delete(&child);
		if (removed)
			volume->error = removed;
	}
	result = inode_put(&child, result);
put_parent:
	result = node_update_end(node, &parent, result);
	if (result) {
		if (created && !PTRISERR(created))
			node_release(created);
		return ERR2PTR(-result);
	}
	return created;
}
static int node_unlink(struct disk_node *node, const char *name, size_t length,
		unsigned int flags, unsigned long mount_flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref parent, child;
	uint32_t number;
	int result;

	if ((mount_flags & MS_RDONLY) || volume->fs.read_only)
		return -EROFS;
	result = valid_name(name, length);
	if (result)
		return -result;
	result = node_update_begin(node, &parent);
	if (result)
		return -result;
	result = find_inode(&parent, name, length, &number);
	if (result)
		goto put_parent;
	result = ext4_fs_get_inode_ref(&volume->fs, number, &child);
	if (result)
		goto put_parent;
	int directory = ext4_inode_type(&volume->fs.sb, child.inode) == S_IFDIR;
	if (directory && (flags & UKFS_UNLINK_NODIR))
		result = EISDIR;
	else if (!directory && (flags & UKFS_UNLINK_DIR))
		result = ENOTDIR;
	else if (directory)
		result = directory_empty(&child);
	if (!result)
		result = ext4_dir_remove_entry(&parent, name, length);
	if (!result) {
		result = drop_link(volume, &child, &parent);
		inode_touch(&parent, 1);
	}
	result = inode_put(&child, result);
put_parent:
	result = node_update_end(node, &parent, result);
	return -result;
}
static int directory_parent(struct ext4_inode_ref *child,
		struct ext4_inode_ref *parent)
{
	if (ext4_inode_has_flag(child->inode, EXT4_INODE_FLAG_INDEX))
		return ext4_dir_dx_reset_parent_inode(child, parent->index);
	struct ext4_dir_search_result entry;
	int result = ext4_dir_find_entry(&entry, child, "..", 2);
	if (result)
		return result;
	ext4_dir_en_set_inode(entry.dentry, parent->index);
	ext4_dir_set_csum(child, (struct ext4_dir_en *)entry.block.data);
	result = ext4_trans_set_block_dirty(entry.block.buf);
	int released = ext4_dir_destroy_result(child, &entry);
	return result ? result : released;
}
static int check_ancestor(struct disk_volume *volume, uint32_t ancestor, uint32_t number)
{
	uint32_t limit = ext4_get32(&volume->fs.sb, inodes_count);
	for (uint32_t depth = 0; depth < limit; depth++) {
		struct ext4_inode_ref ref;
		uint32_t parent;
		if (number == ancestor)
			return EINVAL;
		if (number == EXT4_INODE_ROOT_INDEX)
			return 0;
		int result = ext4_fs_get_inode_ref(&volume->fs, number, &ref);
		if (result)
			return result;
		result = find_inode(&ref, "..", 2, &parent);
		result = inode_put(&ref, result);
		if (result)
			return result;
		if (number == parent)
			return EIO;
		number = parent;
	}
	return ELOOP;
}
static int node_rename(struct disk_node *node, const char *name, size_t length,
		struct disk_node *target, const char *new_name, size_t new_length,
		unsigned int flags, unsigned long mount_flags)
{
	struct disk_volume *volume = node->volume;
	struct ext4_inode_ref parent, destination, child, replaced;
	uint32_t number, old_number;
	int result, have_replaced = 0;

	if (target->volume != volume)
		return -EXDEV;
	if ((mount_flags & MS_RDONLY) || volume->fs.read_only)
		return -EROFS;
	if (flags & ~1U)
		return -EOPNOTSUPP;
	result = valid_name(name, length);
	if (!result)
		result = valid_name(new_name, new_length);
	if (result)
		return -result;
	result = node_update_begin(node, &parent);
	if (result)
		return -result;
	result = ext4_fs_get_inode_ref(&volume->fs, target->inode, &destination);
	if (result)
		goto put_parent;
	if (!ext4_inode_get_links_cnt(parent.inode) ||
	    !ext4_inode_get_links_cnt(destination.inode)) {
		result = ENOENT;
		goto put_destination;
	}
	result = find_inode(&parent, name, length, &number);
	if (result)
		goto put_destination;
	result = ext4_fs_get_inode_ref(&volume->fs, number, &child);
	if (result)
		goto put_destination;
	int directory = ext4_inode_type(&volume->fs.sb, child.inode) == S_IFDIR;
	if (directory) {
		result = check_ancestor(volume, number, target->inode);
		if (result)
			goto put_child;
	}
	result = find_inode(&destination, new_name, new_length, &old_number);
	if (!result) {
		if (flags & 1U) {
			result = EEXIST;
			goto put_child;
		}
		if (old_number == number)
			goto put_child;
		result = ext4_fs_get_inode_ref(&volume->fs, old_number, &replaced);
		if (result)
			goto put_child;
		have_replaced = 1;
		int old_directory = ext4_inode_type(&volume->fs.sb, replaced.inode) == S_IFDIR;
		if (old_directory != directory)
			result = old_directory ? EISDIR : ENOTDIR;
		else if (old_directory)
			result = directory_empty(&replaced);
		if (result)
			goto put_replaced;
		result = ext4_dir_remove_entry(&destination, new_name, new_length);
		if (result)
			goto put_replaced;
	} else if (result != ENOENT) {
		goto put_child;
	}
	result = ext4_dir_add_entry(&destination, new_name, new_length, &child);
	if (!result)
		result = ext4_dir_remove_entry(&parent, name, length);
	if (!result && have_replaced)
		result = drop_link(volume, &replaced, &destination);
	if (!result && directory && node != target) {
		result = directory_parent(&child, &destination);
		if (!result) {
			ext4_fs_inode_links_count_dec(&parent);
			ext4_fs_inode_links_count_inc(&destination);
		}
	}
	if (!result) {
		inode_touch(&child, 0);
		inode_touch(&parent, 1);
		inode_touch(&destination, 1);
	}
put_replaced:
	if (have_replaced)
		result = inode_put(&replaced, result);
put_child:
	result = inode_put(&child, result);
put_destination:
	result = inode_put(&destination, result);
put_parent:
	result = node_update_end(node, &parent, result);
	return -result;
}
static struct uk_fs_path node_readlink(struct disk_node *node)
{
	return (struct uk_fs_path) { .s = node->link, .len = node->link_length };
}
static int node_kind(const struct disk_node *node, enum uk_fs_tmpl_node_kind kind)
{
	return node->type == (kind == UK_FS_TMPL_DIR ? S_IFDIR : S_IFLNK);
}
static struct uk_file_state *node_state(struct disk_node *node)
{
	return &node->state;
}
static int node_error(const struct disk_node *node)
{
	return PTRISERR(node) ? PTR2ERR(node) : 0;
}
static int node_compare(struct disk_node *left, struct disk_node *right)
{
	if (left->volume != right->volume)
		return (uintptr_t)left->volume < (uintptr_t)right->volume ? -1 : 1;
	return left->inode < right->inode ? -1 : left->inode != right->inode;
}
UK_FS_TMPL_LIVE_OPSTABLE(ext4, ext4_live_ops);
static const struct ext4_live_ops ext4_ops = {
	.live_vopen = volume_open, .live_read = node_read, .live_write = node_write,
	.live_mem = node_mem, .live_getstat = node_stat, .live_setstat = node_setstat,
	.live_ctl = node_control, .live_fs_stat = volume_stat, .live_fs_sync = volume_sync,
	.live_fs_lookup = node_lookup, .live_fs_listdir = node_list,
	.live_fs_create = node_create, .live_fs_unlink = node_unlink,
	.live_fs_rename = node_rename, .live_fs_readlink = node_readlink,
	.live_nodekind = node_kind, .live_state = node_state, .live_errnode = node_error,
	.live_acquire = node_acquire, .live_release = node_release,
};
UK_FS_TMPL_LIVE_GENERATE_STATIC(ext4, struct disk_node *, node_compare, ext4_ops, 0);
UK_FS_DRIVER_REGISTER(ext4, UK_FS_TMPL_LIVE_OP_VOPEN(ext4),
		      UK_FS_VOPEN_VOL_RAW | UK_FS_VOPEN_DATA_IGNORE);
