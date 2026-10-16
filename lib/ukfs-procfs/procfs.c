/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <uk/alloc.h>
#include <uk/fs/dirent.h>
#include <uk/fs/driver.h>
#include <uk/fs/template/live.h>
#include <uk/mutex.h>
#include <uk/list.h>
#include <uk/process.h>
#include <uk/posix-fdtab.h>
#include <uk/vfs-path.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif

enum proc_kind {
	PROC_ROOT, PROC_SELF, PROC_PID, PROC_EXE, PROC_FDDIR, PROC_FD, PROC_MAPS
};

struct proc_node {
	struct uk_list_head link;
	struct uk_file_state state;
	struct proc_node *parent;
	struct posix_process *process;
	enum proc_kind kind;
	int descriptor;
	unsigned int references;
};

static UK_LIST_HEAD(nodes);
static struct uk_mutex node_lock = UK_MUTEX_INITIALIZER(node_lock);

UK_FS_TMPL_LIVE_TYPES(procfs, struct proc_node *);
UK_FS_TMPL_LIVE_OPS(procfs, struct proc_node *);

static struct proc_node *node_new(enum proc_kind kind, struct proc_node *parent,
				  struct posix_process *process, int descriptor)
{
	struct proc_node *node;

	uk_mutex_lock(&node_lock);
	uk_list_for_each_entry(node, &nodes, link) {
		if (node->kind == kind && node->parent == parent &&
		    (kind != PROC_PID || node->process == process) &&
		    (kind != PROC_FD || node->descriptor == descriptor)) {
			node->references++;
			goto out;
		}
	}
	node = uk_zalloc(uk_alloc_get_default(), sizeof(*node));
	if (!node)
		goto out;
	node->kind = kind;
	node->parent = parent;
	if (parent)
		parent->references++;
	node->descriptor = descriptor;
	node->process = kind > PROC_PID ? parent->process : process;
	if (kind == PROC_PID)
		process = NULL;
	node->references = 1;
	node->state = (struct uk_file_state)UK_FILE_STATE_INITIALIZER(node->state);
	uk_file_state_event_set(&node->state, POLLIN | POLLOUT);
	uk_list_add_tail(&node->link, &nodes);
out:
	uk_mutex_unlock(&node_lock);
	if (process)
		uk_posix_process_put(process);
	return node ? node : ERR2PTR(-ENOMEM);
}

static void node_acquire(struct proc_node *node)
{
	uk_mutex_lock(&node_lock);
	node->references++;
	uk_mutex_unlock(&node_lock);
}

static void node_release(struct proc_node *node)
{
	struct proc_node *parent;

	uk_mutex_lock(&node_lock);
	if (--node->references) {
		uk_mutex_unlock(&node_lock);
		return;
	}
	uk_list_del_init(&node->link);
	parent = node->parent;
	uk_mutex_unlock(&node_lock);
	if (node->kind == PROC_PID)
		uk_posix_process_put(node->process);
	uk_free(uk_alloc_get_default(), node);
	if (parent)
		node_release(parent);
}

static int node_compare(const struct proc_node *left,
			const struct proc_node *right)
{
	return (__uptr)left < (__uptr)right ? -1 : left == right ? 0 : 1;
}

static int node_error(const struct proc_node *node)
{
	return PTRISERR(node) ? PTR2ERR(node) : 0;
}

static struct uk_file_state *node_state(struct proc_node *node)
{
	return &node->state;
}

static int node_kind(const struct proc_node *node,
		     enum uk_fs_tmpl_node_kind kind)
{
	bool directory = node->kind == PROC_ROOT || node->kind == PROC_PID ||
			 node->kind == PROC_FDDIR;

	return kind == UK_FS_TMPL_DIR ? directory :
		node->kind == PROC_SELF || node->kind == PROC_EXE ||
		node->kind == PROC_FD;
}

static struct proc_node *volume_open(union uk_fs_vopen_vol volume __unused,
				     unsigned long flags __unused,
				     union uk_fs_vopen_data data __unused,
				     size_t format __unused)
{
	return node_new(PROC_ROOT, NULL, NULL, -1);
}

static ssize_t node_io(struct proc_node *node, const struct iovec *iov __unused,
		       size_t count __unused, size_t offset __unused, long flags __unused,
		       unsigned long mount_flags __unused)
{
	return node_kind(node, UK_FS_TMPL_DIR) ? -EISDIR : -EINVAL;
}

#if CONFIG_LIBUKVMEM
static int mapping_line(const struct uk_posix_process_mapping *map,
			char *line, size_t capacity)
{
	struct uk_statx metadata = {0};
	struct uk_fs_path path = {0};
	const char *name = map->name;
	size_t length = name ? strlen(name) : 0;
	int used, rc = 0;

	if (map->file) {
		uk_file_rlock(map->file->file);
		rc = uk_file_getstat(map->file->file, UK_STATX_BASIC_STATS,
				     &metadata);
		uk_file_runlock(map->file->file);
		if (rc)
			return rc;
		path = uk_posix_vfs_ofile_name(map->file);
		if (PTRISERR(path.s))
			return PTR2ERR(path.s);
		name = path.s;
		length = path.len;
	}
	used = snprintf(line, capacity, "%lx-%lx %c%c%c%c %08lx %02x:%02x %llu",
			(unsigned long)map->start, (unsigned long)map->end,
			map->attr & UK_PAGING_PAGE_ATTR_PROT_READ ? 'r' : '-',
			map->attr & UK_PAGING_PAGE_ATTR_PROT_WRITE ? 'w' : '-',
			map->attr & UK_PAGING_PAGE_ATTR_PROT_EXEC ? 'x' : '-',
			map->private ? 'p' : 's', (unsigned long)map->offset,
			metadata.stx_dev_major, metadata.stx_dev_minor,
			(unsigned long long)metadata.stx_ino);
	if (used < 0 || (size_t)used >= capacity) {
		rc = -EOVERFLOW;
		goto out;
	}
	if (length && (size_t)used + 1 < capacity)
		line[used++] = ' ';
	for (size_t index = 0; index < length; index++) {
		size_t bytes = name[index] == '\n' ? 4 : 1;

		if (bytes >= capacity - used) {
			rc = -ENAMETOOLONG;
			goto out;
		}
		if (bytes == 4) {
			memcpy(line + used, "\\012", 4);
			used += 4;
		} else {
			line[used++] = name[index];
		}
	}
	if ((size_t)used + 1 >= capacity) {
		rc = -EOVERFLOW;
		goto out;
	}
	line[used++] = '\n';
	rc = used;
out:
	if (path.s)
		uk_fs_path_release(path);
	return rc;
}

static ssize_t maps_read(struct proc_node *node, const struct iovec *iov,
			 size_t count, size_t offset)
{
	struct uk_posix_process_mapping *maps;
	size_t length, vector = 0, position = 0, written = 0;
	const size_t capacity = 4 * PATH_MAX + 160;
	char *line;
	int rc = uk_posix_process_maps(node->process, &maps, &length);

	if (rc)
		return rc;
	line = uk_malloc(uk_alloc_get_default(), capacity);
	if (!line) {
		rc = -ENOMEM;
		goto out;
	}
	for (size_t index = 0; index < length && vector < count; index++) {
		rc = mapping_line(&maps[index], line, capacity);
		if (rc < 0)
			goto out;
		if (offset >= (size_t)rc) {
			offset -= rc;
			continue;
		}
		size_t cursor = offset;

		offset = 0;
		while (cursor < (size_t)rc && vector < count) {
			size_t bytes = MIN((size_t)rc - cursor,
					   iov[vector].iov_len - position);

			memcpy((char *)iov[vector].iov_base + position,
			       line + cursor, bytes);
			written += bytes;
			cursor += bytes;
			position += bytes;
			if (position == iov[vector].iov_len) {
				vector++;
				position = 0;
			}
		}
	}
out:
	uk_free(uk_alloc_get_default(), line);
	uk_posix_process_maps_release(maps, length);
	return written ? (ssize_t)written : rc < 0 ? rc : 0;
}
#endif

static ssize_t node_read(struct proc_node *node, const struct iovec *iov,
			 size_t count, size_t offset, long flags,
			 unsigned long mount_flags)
{
#if CONFIG_LIBUKVMEM
	if (node->kind == PROC_MAPS)
		return maps_read(node, iov, count, offset);
#endif
	return node_io(node, iov, count, offset, flags, mount_flags);
}

static ssize_t node_mem(struct proc_node *node __unused,
			enum uk_file_mem_op operation __unused, size_t offset __unused,
			size_t length __unused, struct iovec *iov __unused, size_t count __unused,
			unsigned long flags __unused)
{
	return -EOPNOTSUPP;
}

static __u64 node_inode(enum proc_kind kind, struct posix_process *process)
{
	return kind <= PROC_SELF ? kind + 1 : ((__uptr)process >> 4) * 8 + kind;
}

static struct uk_ofile *node_descriptor(struct proc_node *node)
{
	struct uk_fdtab *table = uk_posix_process_fdtable(node->process);
	struct uk_ofile *file;

	if (!table)
		return NULL;
	file = uk_fdtab_get_from(table, node->descriptor);
	uk_fdtab_put(table);
	return file;
}

static int node_stat(struct proc_node *node, unsigned int mask __unused,
		     struct uk_statx *metadata, unsigned long flags __unused)
{
	memset(metadata, 0, sizeof(*metadata));
	metadata->stx_mask = UK_STATX_BASIC_STATS;
	metadata->stx_mode = node_kind(node, UK_FS_TMPL_DIR) ?
		S_IFDIR | (node->kind == PROC_FDDIR ? 0500 : 0555) :
		S_IFLNK | 0777;
	metadata->stx_nlink = node_kind(node, UK_FS_TMPL_DIR) ? 2 : 1;
	metadata->stx_ino = node_inode(node->kind, node->process);
	if (node->kind == PROC_FD) {
		struct uk_ofile *file = node_descriptor(node);

		if (!file)
			return -ENOENT;
		metadata->stx_ino = node_inode(PROC_FD, node->process) *
			CONFIG_LIBPOSIX_FDTAB_MAXFDS + node->descriptor;
		metadata->stx_mode = S_IFLNK;
		if (!(file->mode & O_PATH)) {
			if ((file->mode & O_ACCMODE) != O_WRONLY)
				metadata->stx_mode |= 0500;
			if ((file->mode & O_ACCMODE) != O_RDONLY)
				metadata->stx_mode |= 0300;
		}
		uk_ofile_release(file);
	}
	metadata->stx_blksize = 4096;
	if (node->kind == PROC_MAPS)
		metadata->stx_mode = S_IFREG | 0444;
	return 0;
}

static int node_setstat(struct proc_node *node __unused,
			unsigned int mask __unused, const struct uk_statx *metadata __unused,
			unsigned long flags __unused)
{
	return -EROFS;
}

static int node_control(struct proc_node *node __unused, int family,
			int request, uintptr_t first __unused, uintptr_t second __unused,
			uintptr_t third __unused, unsigned long flags __unused)
{
	if (family == UKFILE_CTL_FILE && request == UKFILE_CTL_FILE_SYNC)
		return 0;
	return family == UKFILE_CTL_IOCTL ? -ENOTTY : -EROFS;
}

static int volume_stat(struct proc_node *node __unused, struct statfs *metadata)
{
	memset(metadata, 0, sizeof(*metadata));
	metadata->f_type = 0x9fa0;
	metadata->f_bsize = 4096;
	metadata->f_namelen = NAME_MAX;
	return 0;
}

static int volume_sync(struct proc_node *node __unused)
{
	return 0;
}

static int node_number(const char *name, size_t length)
{
	int number = 0;

	for (size_t index = 0; index < length; index++) {
		if (name[index] < '0' || name[index] > '9' ||
		    number > (INT_MAX - (name[index] - '0')) / 10)
			return -ENOENT;
		number = number * 10 + name[index] - '0';
	}
	return number;
}

static int node_lookup(struct proc_node *node, const char *name, size_t length,
		       struct proc_node **output, union uk_fs_lookup_out *details __unused,
		       size_t *consumed)
{
	struct proc_node *found;
	struct posix_process *process;
	pid_t pid = 0;
	size_t part = 0;

	if ((node->kind == PROC_PID || node->kind == PROC_FDDIR) &&
	    !uk_posix_process_attached(node->process))
		return -ESRCH;
	while (part < length && name[part] != '/')
		part++;
	if (!part)
		return -ENOENT;
	if (part == 1 && name[0] == '.') {
		found = node;
		node_acquire(found);
	} else if (part == 2 && !memcmp(name, "..", 2)) {
		found = node->parent ? node->parent : node;
		node_acquire(found);
	} else if (node->kind == PROC_ROOT && part == 4 &&
		   !memcmp(name, "self", 4)) {
		found = node_new(PROC_SELF, node, NULL, -1);
	} else if (node->kind == PROC_PID && part == 3 &&
		   !memcmp(name, "exe", 3)) {
		found = node_new(PROC_EXE, node, NULL, -1);
	} else if (node->kind == PROC_PID && part == 2 &&
		   !memcmp(name, "fd", 2)) {
		found = node_new(PROC_FDDIR, node, NULL, -1);
#if CONFIG_LIBUKVMEM
	} else if (node->kind == PROC_PID && part == 4 &&
		   !memcmp(name, "maps", 4)) {
		found = node_new(PROC_MAPS, node, NULL, -1);
#endif
	} else if (node->kind == PROC_FDDIR) {
		struct uk_fdtab *table;
		struct uk_ofile *file;

		if (part > 1 && name[0] == '0')
			return -ENOENT;
		pid = node_number(name, part);
		if (pid < 0)
			return pid;
		table = uk_posix_process_fdtable(node->process);
		if (!table)
			return -ENOENT;
		file = uk_fdtab_get_from(table, pid);
		uk_fdtab_put(table);
		if (!file)
			return -ENOENT;
		uk_ofile_release(file);
		found = node_new(PROC_FD, node, NULL, pid);
	} else if (node->kind == PROC_ROOT) {
		if (name[0] == '0')
			return -ENOENT;
		pid = node_number(name, part);
		if (pid < 0)
			return pid;
		process = uk_posix_process_get(pid);
		if (!process)
			return -ENOENT;
		found = node_new(PROC_PID, node, process, -1);
	} else {
		return -ENOENT;
	}
	if (PTRISERR(found))
		return PTR2ERR(found);
	output[0] = found;
	*consumed = part;
	if (node_kind(found, UK_FS_TMPL_SYM)) {
		node_acquire(node);
		output[1] = node;
		return UKFS_STOP_SYM;
	}
	return UKFS_STOP_NOD;
}

static ssize_t node_list(struct proc_node *node, size_t *cursor, void *buffer,
			 size_t size, unsigned long flags __unused)
{
	struct uk_fs_dirent *entry;
	struct posix_process *process;
	const char *name;
	char number[24];
	size_t next, length, record, used = 0;
	unsigned char type;
	enum proc_kind kind;
	__u64 inode;
	pid_t pid;

	if ((node->kind == PROC_PID || node->kind == PROC_FDDIR) &&
	    !uk_posix_process_attached(node->process))
		return 0;
	for (;;) {
		next = *cursor + 1;
		type = DT_DIR;
		inode = node_inode(node->kind, node->process);
		if (*cursor < 2) {
			struct proc_node *directory = *cursor && node->parent ?
				node->parent : node;

			name = *cursor ? ".." : ".";
			inode = node_inode(directory->kind, directory->process);
		} else if (node->kind == PROC_FDDIR) {
			struct uk_fdtab *table;
			int descriptor;

			if (*cursor - 2 > INT_MAX)
				break;
			table = uk_posix_process_fdtable(node->process);
			if (!table)
				break;
			descriptor = uk_fdtab_next(table, *cursor - 2);
			uk_fdtab_put(table);
			if (descriptor < 0)
				break;
			snprintf(number, sizeof(number), "%d", descriptor);
			name = number;
			type = DT_LNK;
			next = (size_t)descriptor + 3;
			inode = node_inode(PROC_FD, node->process) *
				CONFIG_LIBPOSIX_FDTAB_MAXFDS + descriptor;
		} else if (node->kind == PROC_PID) {
#if CONFIG_LIBUKVMEM
			if (*cursor >= 5)
#else
			if (*cursor >= 4)
#endif
				break;
			name = *cursor == 2 ? "exe" :
				*cursor == 3 ? "fd" : "maps";
			type = *cursor == 2 ? DT_LNK :
				*cursor == 3 ? DT_DIR : DT_REG;
			kind = *cursor == 2 ? PROC_EXE :
				*cursor == 3 ? PROC_FDDIR : PROC_MAPS;
			inode = node_inode(kind, node->process);
		} else if (*cursor == 2) {
			name = "self";
			type = DT_LNK;
			inode = node_inode(PROC_SELF, NULL);
		} else {
			pid = uk_posix_process_next_pid(*cursor - 3);
			if (!pid)
				break;
			process = uk_posix_process_get(pid);
			if (!process) {
				*cursor = pid + 3;
				continue;
			}
			inode = node_inode(PROC_PID, process);
			uk_posix_process_put(process);
			snprintf(number, sizeof(number), "%d", pid);
			name = number;
			next = pid + 3;
		}
		length = strlen(name);
		record = UKFS_DIRENT_RECLEN(length);
		if (record > size - used)
			return used ? (ssize_t)used : -EINVAL;
		entry = (void *)((char *)buffer + used);
		memset(entry, 0, record);
		entry->d_ino = inode;
		entry->d_off = next;
		entry->d_reclen = record;
		entry->d_type = type;
		memcpy(entry->d_name, name, length);
		used += record;
		*cursor = next;
	}
	return used;
}

static struct proc_node *node_create(struct proc_node *node __unused,
				     const char *name __unused, size_t length __unused,
				     unsigned int mode __unused, int flags __unused,
				     union UK_FS_TMPL_LIVE_CREATE_TARGET(procfs) target __unused,
				     unsigned long mount_flags __unused)
{
	return ERR2PTR(-EROFS);
}

static int node_unlink(struct proc_node *node __unused,
		       const char *name __unused, size_t length __unused,
		       unsigned int flags __unused, unsigned long mount_flags __unused)
{
	return -EROFS;
}

static int node_rename(struct proc_node *source __unused,
		       const char *name __unused, size_t length __unused,
		       struct proc_node *destination __unused, const char *newname __unused,
		       size_t newlength __unused, unsigned int flags __unused,
		       unsigned long mount_flags __unused)
{
	return -EROFS;
}

static struct uk_fs_path node_readlink(struct proc_node *node)
{
	struct uk_vfs_path *path;
	struct uk_fs_path name;
	char number[24];
	int length;

	if (node->kind == PROC_SELF) {
		length = snprintf(number, sizeof(number), "%d", uk_sys_getpid());
		return uk_fs_path_copy(number, length, uk_alloc_get_default());
	}
	if (node->kind == PROC_FD) {
		struct uk_ofile *file = node_descriptor(node);

		if (!file)
			return (struct uk_fs_path){.s = ERR2PTR(-ENOENT)};
		name = uk_posix_vfs_ofile_name(file);
		uk_ofile_release(file);
		return name;
	}
	path = uk_posix_process_executable(node->process);
	if (PTRISERR(path))
		return (struct uk_fs_path){.s = ERR2PTR(PTR2ERR(path))};
	name = uk_posix_vfs_path_name(path);
	uk_posix_vfs_path_release(path);
	return name;
}

static const struct uk_file *node_follow(const struct uk_file *file);

UK_FS_TMPL_LIVE_OPSTABLE(procfs, procfs_live_ops);
static const struct procfs_live_ops procfs_ops = {
	.live_vopen = volume_open,
	.live_read = node_read, .live_write = node_io,
	.live_mem = node_mem, .live_getstat = node_stat, .live_setstat = node_setstat,
	.live_ctl = node_control, .live_fs_stat = volume_stat,
	.live_fs_sync = volume_sync, .live_fs_lookup = node_lookup,
	.live_fs_listdir = node_list, .live_fs_create = node_create,
	.live_fs_unlink = node_unlink, .live_fs_rename = node_rename,
	.live_fs_readlink = node_readlink, .live_nodekind = node_kind,
	.live_state = node_state, .live_errnode = node_error,
	.live_acquire = node_acquire, .live_release = node_release
};

UK_FS_TMPL_LIVE_GENERATE_STATIC(procfs, struct proc_node *, node_compare,
				procfs_ops, UKFS_MODE_NOIOLOCK, node_follow);

static const struct uk_file *node_follow(const struct uk_file *file)
{
	struct proc_node *node = procfs_FILE_NODE(file);
	struct uk_vfs_path *path;
	const struct uk_file *target;

	if (node->kind == PROC_SELF || node->kind == PROC_MAPS)
		return NULL;
	if (node->kind == PROC_FD) {
		struct uk_ofile *descriptor = node_descriptor(node);

		if (!descriptor)
			return ERR2PTR(-ENOENT);
		target = descriptor->file;
		uk_file_acquire(target);
		uk_ofile_release(descriptor);
		return target;
	}
	path = uk_posix_process_executable(node->process);
	if (PTRISERR(path))
		return ERR2PTR(PTR2ERR(path));
	target = uk_posix_vfs_path_file(path);
	uk_posix_vfs_path_release(path);
	return target;
}

UK_FS_DRIVER_REGISTER(proc, UK_FS_TMPL_LIVE_OP_VOPEN(procfs),
		      UK_FS_VOPEN_VOL_IGNORE | UK_FS_VOPEN_DATA_IGNORE);
