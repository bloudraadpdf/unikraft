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
#include <uk/vfs-path.h>

enum proc_kind {
	PROC_ROOT, PROC_SELF, PROC_PID, PROC_EXE
};

struct proc_node {
	struct uk_list_head link;
	struct uk_file_state state;
	struct proc_node *parent;
	struct posix_process *process;
	enum proc_kind kind;
	unsigned int references;
};

static UK_LIST_HEAD(nodes);
static struct uk_mutex node_lock = UK_MUTEX_INITIALIZER(node_lock);

UK_FS_TMPL_LIVE_TYPES(procfs, struct proc_node *);
UK_FS_TMPL_LIVE_OPS(procfs, struct proc_node *);

static struct proc_node *node_new(enum proc_kind kind, struct proc_node *parent,
				  struct posix_process *process)
{
	struct proc_node *node;

	uk_mutex_lock(&node_lock);
	uk_list_for_each_entry(node, &nodes, link) {
		if (node->kind == kind && node->parent == parent &&
		    (kind != PROC_PID || node->process == process)) {
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
	node->process = kind == PROC_EXE ? parent->process : process;
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
	bool directory = node->kind == PROC_ROOT || node->kind == PROC_PID;

	return kind == UK_FS_TMPL_DIR ? directory : !directory;
}

static struct proc_node *volume_open(union uk_fs_vopen_vol volume __unused,
				     unsigned long flags __unused,
				     union uk_fs_vopen_data data __unused,
				     size_t format __unused)
{
	return node_new(PROC_ROOT, NULL, NULL);
}

static ssize_t node_io(struct proc_node *node, const struct iovec *iov __unused,
		       size_t count __unused, size_t offset __unused, long flags __unused,
		       unsigned long mount_flags __unused)
{
	return node_kind(node, UK_FS_TMPL_DIR) ? -EISDIR : -EINVAL;
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
	return kind <= PROC_SELF ? kind + 1 : ((__uptr)process >> 4) * 4 + kind;
}

static int node_stat(struct proc_node *node, unsigned int mask __unused,
		     struct uk_statx *metadata, unsigned long flags __unused)
{
	memset(metadata, 0, sizeof(*metadata));
	metadata->stx_mask = UK_STATX_BASIC_STATS;
	metadata->stx_mode = node_kind(node, UK_FS_TMPL_DIR) ?
		S_IFDIR | 0555 : S_IFLNK | 0777;
	metadata->stx_nlink = node_kind(node, UK_FS_TMPL_DIR) ? 2 : 1;
	metadata->stx_ino = node_inode(node->kind, node->process);
	metadata->stx_blksize = 4096;
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

static int node_lookup(struct proc_node *node, const char *name, size_t length,
		       struct proc_node **output, union uk_fs_lookup_out *details __unused,
		       size_t *consumed)
{
	struct proc_node *found;
	struct posix_process *process;
	pid_t pid = 0;
	size_t part = 0;

	if (node->kind == PROC_PID &&
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
		found = node_new(PROC_SELF, node, NULL);
	} else if (node->kind == PROC_PID && part == 3 &&
		   !memcmp(name, "exe", 3)) {
		found = node_new(PROC_EXE, node, NULL);
	} else if (node->kind == PROC_ROOT) {
		if (name[0] == '0')
			return -ENOENT;
		for (size_t index = 0; index < part; index++) {
			if (name[index] < '0' || name[index] > '9' ||
			    pid > (INT_MAX - (name[index] - '0')) / 10)
				return -ENOENT;
			pid = pid * 10 + name[index] - '0';
		}
		process = uk_posix_process_get(pid);
		if (!process)
			return -ENOENT;
		found = node_new(PROC_PID, node, process);
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
	__u64 inode;
	pid_t pid;

	if (node->kind == PROC_PID &&
	    !uk_posix_process_attached(node->process))
		return 0;
	for (;;) {
		next = *cursor + 1;
		type = DT_DIR;
		inode = node_inode(node->kind, node->process);
		if (*cursor < 2) {
			name = *cursor ? ".." : ".";
		} else if (node->kind == PROC_PID) {
			if (*cursor != 2)
				break;
			name = "exe";
			type = DT_LNK;
			inode = node_inode(PROC_EXE, node->process);
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
	.live_vopen = volume_open, .live_read = node_io, .live_write = node_io,
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

	if (node->kind == PROC_SELF)
		return NULL;
	path = uk_posix_process_executable(node->process);
	if (PTRISERR(path))
		return ERR2PTR(PTR2ERR(path));
	target = uk_posix_vfs_path_file(path);
	uk_posix_vfs_path_release(path);
	return target;
}

UK_FS_DRIVER_REGISTER(proc, UK_FS_TMPL_LIVE_OP_VOPEN(procfs),
		      UK_FS_VOPEN_VOL_IGNORE | UK_FS_VOPEN_DATA_IGNORE);
