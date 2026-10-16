/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <erofs/internal.h>
#include <erofs/dir.h>
#include <uk/file.h>
#include <uk/blkfile.h>
#include <uk/fs.h>
#include <uk/fs/dirent.h>
#include <uk/fs/driver.h>
#include <uk/fs/template/live.h>
#include <uk/mutex.h>

#define EROFS_READ_CACHE_BYTES (64 * 1024)

struct erofs_cached_block {
	u64 offset;
	size_t length;
	unsigned char data[EROFS_READ_CACHE_BYTES];
};

struct erofs_volume {
	struct erofs_sb_info super;
	struct uk_fs_vopen_memimg image;
	const struct uk_file *source;
	struct uk_mutex lock;
	unsigned int references;
	/* After mount, the volume lock protects cached reads. */
	struct erofs_cached_block cache[64];
};

struct erofs_node {
	struct erofs_volume *volume;
	struct erofs_inode inode;
	struct uk_file_state state;
	char *link;
	unsigned int references;
};

UK_FS_TMPL_LIVE_TYPES(erofs, struct erofs_node *);
UK_FS_TMPL_LIVE_OPS(erofs, struct erofs_node *);

static ssize_t image_read(struct erofs_vfile *file, void *buffer,
			  size_t length, u64 offset)
{
	struct erofs_volume *volume;

	memcpy(&volume, file->payload, sizeof(volume));
	if (offset > volume->image.len ||
	    length > volume->image.len - offset)
		return -EIO;
	if (volume->source) {
		size_t copied = 0;

		while (length) {
			u64 aligned = round_down(offset, EROFS_READ_CACHE_BYTES);
			struct erofs_cached_block *block =
				&volume->cache[(aligned / EROFS_READ_CACHE_BYTES) %
					       ARRAY_SIZE(volume->cache)];
			size_t prefix = offset - aligned;
			size_t count;

			if (!block->length || block->offset != aligned) {
				const struct iovec vector = {
					.iov_base = block->data,
					.iov_len = MIN(sizeof(block->data),
						       volume->image.len - aligned)
				};
				ssize_t result;

				block->length = 0;
				result = uk_file_read(volume->source, &vector, 1,
						      aligned, 0);
				if (result < 0)
					return result;
				if ((size_t)result != vector.iov_len)
					return -EIO;
				block->offset = aligned;
				block->length = result;
			}
			count = MIN(length, block->length - prefix);
			memcpy((char *)buffer + copied, block->data + prefix, count);
			copied += count;
			offset += count;
			length -= count;
		}

		return copied;
	}
	memcpy(buffer, (const char *)volume->image.base + offset, length);
	return length;
}

static struct erofs_vfops image_ops = { .pread = image_read };

static void volume_release(struct erofs_volume *volume)
{
	if (__atomic_sub_fetch(&volume->references, 1, __ATOMIC_ACQ_REL))
		return;
	if (volume->source)
		uk_file_release(volume->source);
	free(volume->super.devs);
	free(volume);
}

static void node_acquire(struct erofs_node *node)
{
	__atomic_add_fetch(&node->references, 1, __ATOMIC_RELAXED);
}

static void node_release(struct erofs_node *node)
{
	struct erofs_volume *volume = node->volume;

	if (__atomic_sub_fetch(&node->references, 1, __ATOMIC_ACQ_REL))
		return;
	free(node->link);
	free(node);
	volume_release(volume);
}

static struct erofs_node *new_node(struct erofs_volume *volume,
				 struct erofs_inode *inode)
{
	struct erofs_node *node = calloc(1, sizeof(*node));
	struct erofs_vfile file;
	int result;

	if (!node)
		return ERR2PTR(-ENOMEM);
	node->volume = volume;
	node->inode = *inode;
	node->references = 1;
	node->state = (struct uk_file_state)
		UK_FILE_STATE_INITIALIZER(node->state);
	__atomic_add_fetch(&volume->references, 1, __ATOMIC_RELAXED);
	if (S_ISLNK(inode->i_mode)) {
		if (inode->i_size > PATH_MAX) {
			result = -ENAMETOOLONG;
			goto failure;
		}
		node->link = malloc(inode->i_size + 1);
		if (!node->link) {
			result = -ENOMEM;
			goto failure;
		}
		result = erofs_iopen(&file, &node->inode);
		if (!result)
			result = erofs_pread(&file, node->link, inode->i_size, 0);
		if (result)
			goto failure;
		node->link[inode->i_size] = '\0';
	}
	uk_file_state_event_set(&node->state, POLLIN | POLLOUT);
	return node;
failure:
	node_release(node);
	return ERR2PTR(result);
}

static struct erofs_node *volume_open(union uk_fs_vopen_vol input,
		unsigned long flags __unused, union uk_fs_vopen_data data __unused,
		size_t format)
{
	struct erofs_volume *volume;
	struct erofs_inode inode;
	struct erofs_node *root;
	struct uk_statx metadata;
	int result;

	volume = calloc(1, sizeof(*volume));
	if (!volume)
		return ERR2PTR(-ENOMEM);
	volume->references = 1;
	switch (format & UK_FS_VOPEN_VOL_MASK) {
	case UK_FS_VOPEN_VOL_MEMIMG:
		if (!input.memimg || !input.memimg->base ||
		    input.memimg->len < 4096) {
			result = -EINVAL;
			goto failure;
		}
		volume->image = *input.memimg;
		break;
	case UK_FS_VOPEN_VOL_RAW:
		volume->source = uk_blkfile_open(input.raw);
		if (PTRISERR(volume->source)) {
			result = PTR2ERR(volume->source);
			volume->source = NULL;
			goto failure;
		}
		uk_file_acquire(volume->source);
		result = uk_file_getstat(volume->source, UK_STATX_SIZE,
					&metadata);
		if (result)
			goto failure;
		if (metadata.stx_size < 4096 || metadata.stx_size > SIZE_MAX) {
			result = -EINVAL;
			goto failure;
		}
		volume->image.len = metadata.stx_size;
		break;
	default:
		result = -EINVAL;
		goto failure;
	}
	volume->super.bdev.ops = &image_ops;
	memcpy(volume->super.bdev.payload, &volume, sizeof(volume));
	uk_mutex_init(&volume->lock);
	result = erofs_read_superblock(&volume->super);
	if (!result && volume->super.extra_devices)
		result = -EOPNOTSUPP;
	if (!result && erofs_sb_has_sb_chksum(&volume->super))
		result = erofs_superblock_csum_verify(&volume->super);
	if (result)
		goto failure;
	inode = (struct erofs_inode) {
		.sbi = &volume->super, .nid = volume->super.root_nid
	};
	result = erofs_read_inode_from_disk(&inode);
	if (result)
		goto failure;
	if (!S_ISDIR(inode.i_mode)) {
		result = -ENOTDIR;
		goto failure;
	}
	root = new_node(volume, &inode);
	if (PTRISERR(root)) {
		result = PTR2ERR(root);
		goto failure;
	}
	volume_release(volume);
	return root;
failure:
	volume_release(volume);
	return ERR2PTR(result);
}

static ssize_t node_read(struct erofs_node *node, const struct iovec *iov,
		size_t count, size_t offset, long flags __unused,
		unsigned long mount_flags __unused)
{
	struct erofs_vfile file;
	size_t length, copied = 0;
	int result;

	if (S_ISDIR(node->inode.i_mode))
		return -EISDIR;
	uk_mutex_lock(&node->volume->lock);
	result = erofs_iopen(&file, &node->inode);
	if (result)
		goto out;
	for (size_t i = 0; i < count && offset < node->inode.i_size; ++i) {
		length = MIN(iov[i].iov_len, node->inode.i_size - offset);
		result = erofs_pread(&file, iov[i].iov_base, length, offset);
		if (result)
			goto out;
		offset += length;
		copied += length;
	}
out:
	uk_mutex_unlock(&node->volume->lock);
	return copied ? (ssize_t)copied : result;
}

static ssize_t node_write(struct erofs_node *node __unused,
		const struct iovec *iov __unused, size_t count __unused,
		size_t offset __unused, long flags __unused,
		unsigned long mount_flags __unused)
{
	return -EROFS;
}

static ssize_t node_mem(struct erofs_node *node __unused,
		enum uk_file_mem_op op __unused, size_t offset __unused,
		size_t length __unused, struct iovec *iov __unused,
		size_t count __unused, unsigned long flags __unused)
{
	return -EOPNOTSUPP;
}

static int node_stat(struct erofs_node *node, unsigned int mask __unused,
		struct uk_statx *stat, unsigned long flags __unused)
{
	const struct erofs_inode *inode = &node->inode;

	memset(stat, 0, sizeof(*stat));
	stat->stx_mask = UK_STATX_BASIC_STATS;
	stat->stx_blksize = erofs_blksiz(inode->sbi);
	stat->stx_mode = inode->i_mode;
	stat->stx_uid = inode->i_uid;
	stat->stx_gid = inode->i_gid;
	stat->stx_nlink = inode->i_nlink;
	stat->stx_ino = inode->nid;
	stat->stx_size = inode->i_size;
	stat->stx_blocks = (inode->i_size + 511) / 512;
	stat->stx_mtime.tv_sec = inode->i_mtime;
	stat->stx_mtime.tv_nsec = inode->i_mtime_nsec;
	stat->stx_ctime = stat->stx_mtime;
	stat->stx_atime = stat->stx_mtime;
	return 0;
}

static int node_setstat(struct erofs_node *node __unused,
		unsigned int mask __unused, const struct uk_statx *stat __unused,
		unsigned long flags __unused)
{
	return -EROFS;
}

static int node_control(struct erofs_node *node __unused, int family,
		int request, uintptr_t arg1 __unused, uintptr_t arg2 __unused,
		uintptr_t arg3 __unused, unsigned long flags __unused)
{
	if (family == UKFILE_CTL_FILE && request == UKFILE_CTL_FILE_SYNC)
		return 0;
	return family == UKFILE_CTL_IOCTL ? -ENOTTY : -EROFS;
}

static int volume_stat(struct erofs_node *node, struct statfs *stat)
{
	memset(stat, 0, sizeof(*stat));
	stat->f_type = EROFS_SUPER_MAGIC_V1;
	stat->f_bsize = erofs_blksiz(&node->volume->super);
	stat->f_blocks = node->volume->super.total_blocks;
	stat->f_files = node->volume->super.inos;
	stat->f_namelen = EROFS_NAME_LEN;
	return 0;
}

static int volume_sync(struct erofs_node *node __unused)
{
	return 0;
}

static int node_lookup(struct erofs_node *node, const char *path, size_t len,
		struct erofs_node **out, union uk_fs_lookup_out *details __unused,
		size_t *consumed)
{
	struct erofs_inode inode;
	struct erofs_node *found = NULL;
	size_t length = 0;
	int result;

	while (length < len && path[length] != '/')
		length++;
	if (!length || length > EROFS_NAME_LEN)
		return -EINVAL;
	uk_mutex_lock(&node->volume->lock);
	result = erofs_ilookupat(&node->inode, path, length, &inode);
	if (!result) {
		found = new_node(node->volume, &inode);
		if (PTRISERR(found))
			result = PTR2ERR(found);
	}
	uk_mutex_unlock(&node->volume->lock);
	if (result)
		return result;
	out[0] = found;
	*consumed = length;
	if (S_ISLNK(inode.i_mode)) {
		node_acquire(node);
		out[1] = node;
		return UKFS_STOP_SYM;
	}
	return UKFS_STOP_NOD;
}

struct list_context {
	struct erofs_dir_context directory;
	size_t index, used, length;
	size_t *cursor;
	char *output;
};

static int emit_directory_entry(struct erofs_dir_context *directory)
{
	static const unsigned char types[] = {
		DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
	};
	struct list_context *context = __containerof(directory,
						  struct list_context, directory);
	struct uk_fs_dirent *entry;
	size_t length = UKFS_DIRENT_RECLEN(directory->de_namelen);

	if (context->index++ < *context->cursor)
		return 0;
	if (length > context->length - context->used)
		return 1;
	entry = (struct uk_fs_dirent *)(context->output + context->used);
	memset(entry, 0, length);
	entry->d_ino = directory->de_nid;
	entry->d_off = context->index;
	entry->d_reclen = length;
	entry->d_type = directory->de_ftype < sizeof(types) ?
			 types[directory->de_ftype] : DT_UNKNOWN;
	memcpy(entry->d_name, directory->dname, directory->de_namelen);
	context->used += length;
	*context->cursor = context->index;
	return 0;
}

static ssize_t node_list(struct erofs_node *node, size_t *cursor,
		void *buffer, size_t length, unsigned long flags __unused)
{
	struct list_context context = {
		.directory = { .dir = &node->inode, .cb = emit_directory_entry },
		.cursor = cursor, .output = buffer, .length = length
	};
	int result;

	uk_mutex_lock(&node->volume->lock);
	result = erofs_iterate_dir(&context.directory, true);
	uk_mutex_unlock(&node->volume->lock);
	if (result < 0)
		return result;
	if (result && !context.used)
		return -EINVAL;
	return context.used;
}

static struct erofs_node *node_create(struct erofs_node *node __unused,
		const char *name __unused, size_t length __unused,
		unsigned int mode __unused, int flags __unused,
		union UK_FS_TMPL_LIVE_CREATE_TARGET(erofs) target __unused,
		unsigned long mount_flags __unused)
{
	return ERR2PTR(-EROFS);
}

static int node_unlink(struct erofs_node *node __unused,
		const char *name __unused, size_t length __unused,
		unsigned int flags __unused, unsigned long mount_flags __unused)
{
	return -EROFS;
}

static int node_rename(struct erofs_node *node __unused,
		const char *name __unused, size_t length __unused,
		struct erofs_node *target __unused, const char *new_name __unused,
		size_t new_length __unused, unsigned int flags __unused,
		unsigned long mount_flags __unused)
{
	return -EROFS;
}

static struct uk_fs_path node_readlink(struct erofs_node *node)
{
	return uk_fs_path_copy(node->link, node->inode.i_size,
			       uk_alloc_get_default());
}

static int node_kind(const struct erofs_node *node,
		     enum uk_fs_tmpl_node_kind kind)
{
	return kind == UK_FS_TMPL_DIR ? S_ISDIR(node->inode.i_mode) :
				       S_ISLNK(node->inode.i_mode);
}

static struct uk_file_state *node_state(struct erofs_node *node)
{
	return &node->state;
}

static int node_error(const struct erofs_node *node)
{
	return PTRISERR(node) ? PTR2ERR(node) : 0;
}

static int node_compare(struct erofs_node *left, struct erofs_node *right)
{
	if (left->volume != right->volume)
		return (uintptr_t)left->volume < (uintptr_t)right->volume ? -1 : 1;
	return left->inode.nid < right->inode.nid ? -1 :
		left->inode.nid == right->inode.nid ? 0 : 1;
}

UK_FS_TMPL_LIVE_OPSTABLE(erofs, erofs_live_ops);
static const struct erofs_live_ops erofs_ops = {
	.live_vopen = volume_open,
	.live_read = node_read, .live_write = node_write, .live_mem = node_mem,
	.live_getstat = node_stat, .live_setstat = node_setstat,
	.live_ctl = node_control,
	.live_fs_stat = volume_stat, .live_fs_sync = volume_sync,
	.live_fs_lookup = node_lookup, .live_fs_listdir = node_list,
	.live_fs_create = node_create, .live_fs_unlink = node_unlink,
	.live_fs_rename = node_rename, .live_fs_readlink = node_readlink,
	.live_nodekind = node_kind, .live_state = node_state,
	.live_errnode = node_error, .live_acquire = node_acquire,
	.live_release = node_release,
};

UK_FS_TMPL_LIVE_GENERATE_STATIC(erofs, struct erofs_node *, node_compare,
			      erofs_ops, 0, NULL);
UK_FS_DRIVER_REGISTER(erofs, UK_FS_TMPL_LIVE_OP_VOPEN(erofs),
		      UK_FS_VOPEN_VOL_MEMIMG | UK_FS_VOPEN_VOL_RAW |
		      UK_FS_VOPEN_DATA_IGNORE);
