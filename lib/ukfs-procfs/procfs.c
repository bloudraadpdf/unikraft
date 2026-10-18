/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <uk/alloc.h>
#include <uk/atomic.h>
#include <uk/file/nops.h>
#include <uk/fs/dirent.h>
#include <uk/fs/driver.h>
#include <uk/fs/template/live.h>
#include <uk/mutex.h>
#include <uk/nofault.h>
#include <uk/list.h>
#include <uk/process.h>
#include <uk/posix-sem.h>
#if CONFIG_LIBPOSIX_USER
#include <uk/credentials.h>
#endif
#include <uk/posix-fdtab.h>
#include <uk/vfs-path.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#endif

enum proc_kind {
	PROC_ROOT,
	PROC_SELF,
	PROC_PID,
	PROC_EXE,
	PROC_FDDIR,
	PROC_FD,
	PROC_MAPS,
	PROC_TASKDIR,
	PROC_TID,
	PROC_COMM,
	PROC_TID_COMM,
	PROC_STATUS,
	PROC_SYS,
	PROC_SYS_KERNEL,
	PROC_SYS_SEM,
	PROC_KINDS
};

struct proc_fixed_entry {
	enum proc_kind parent, kind;
	const char *name;
};

static const struct proc_fixed_entry fixed_entries[] = {
	{PROC_ROOT, PROC_SELF, "self"},
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
	{PROC_ROOT, PROC_SYS, "sys"},
	{PROC_SYS, PROC_SYS_KERNEL, "kernel"},
	{PROC_SYS_KERNEL, PROC_SYS_SEM, "sem"},
#endif
};

static const struct proc_fixed_entry *
fixed_named(enum proc_kind parent, const char *name, size_t length)
{
	for (size_t i = 0; i < ARRAY_SIZE(fixed_entries); i++)
		if (fixed_entries[i].parent == parent &&
		    strlen(fixed_entries[i].name) == length &&
		    !memcmp(fixed_entries[i].name, name, length))
			return &fixed_entries[i];
	return NULL;
}

static const struct proc_fixed_entry *fixed_at(enum proc_kind parent,
					       size_t index)
{
	for (size_t i = 0; i < ARRAY_SIZE(fixed_entries); i++)
		if (fixed_entries[i].parent == parent && !index--)
			return &fixed_entries[i];
	return NULL;
}

struct proc_node {
	struct uk_list_head link;
	struct uk_file_state state;
	struct proc_node *parent;
	struct posix_process *process;
	struct uk_posix_task *task;
	__u64 inode;
	enum proc_kind kind;
	int descriptor;
	unsigned int descriptor_mode;
	bool task_scope;
	unsigned int references;
};

static UK_LIST_HEAD(nodes);
static struct uk_mutex node_lock = UK_MUTEX_INITIALIZER(node_lock);

UK_FS_TMPL_LIVE_TYPES(procfs, struct proc_node *);
UK_FS_TMPL_LIVE_OPS(procfs, struct proc_node *);

static struct proc_node *node_new(enum proc_kind kind, struct proc_node *parent,
				  struct posix_process *process, int descriptor,
				  struct uk_posix_task *task,
				  unsigned int descriptor_mode)
{
	struct proc_node *node;
	int error = -ENOMEM;
	const __u64 stride =
	    2 * ((__u64)PROC_KINDS + CONFIG_LIBPOSIX_FDTAB_MAXFDS);
	bool task_scope = kind == PROC_TID || (parent && parent->task_scope);
	__u64 serial = 0;

	if (!task && parent && parent->task) {
		task = parent->task;
		uk_posix_process_task_acquire(task);
	}
	if (task)
		serial = uk_posix_process_task_serial(task);
	uk_mutex_lock(&node_lock);
	uk_list_for_each_entry(node, &nodes, link) {
		if (node->kind == kind && node->parent == parent &&
		    node->task == task &&
		    (kind != PROC_PID || node->process == process) &&
		    (kind != PROC_FD || node->descriptor == descriptor)) {
			node->references++;
			if (kind == PROC_FD)
				node->descriptor_mode = descriptor_mode;
			goto out;
		}
	}
	if (serial > (UINT64_MAX - stride - 1) / stride) {
		node = NULL;
		error = -EOVERFLOW;
		goto out;
	}
	node = uk_zalloc(uk_alloc_get_default(), sizeof(*node));
	if (!node)
		goto out;
	node->inode =
	    serial * stride + (task_scope ? stride / 2 : 0) +
	    (kind == PROC_FD ? (__u64)PROC_KINDS + descriptor : (__u64)kind) +
	    1;
	node->kind = kind;
	node->parent = parent;
	if (parent)
		parent->references++;
	node->descriptor = descriptor;
	node->descriptor_mode = descriptor_mode;
	node->task_scope = task_scope;
	node->process = kind > PROC_PID ? parent->process : process;
	if (kind == PROC_PID)
		process = NULL;
	node->task = task;
	task = NULL;
	node->references = 1;
	node->state =
	    (struct uk_file_state)UK_FILE_STATE_INITIALIZER(node->state);
	uk_file_state_event_set(&node->state, POLLIN | POLLOUT);
	uk_list_add_tail(&node->link, &nodes);
out:
	uk_mutex_unlock(&node_lock);
	if (process)
		uk_posix_process_put(process);
	if (task)
		uk_posix_process_task_put(task);
	return node ? node : ERR2PTR(error);
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
	if (node->task)
		uk_posix_process_task_put(node->task);
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
			 node->kind == PROC_FDDIR ||
			 node->kind == PROC_TASKDIR || node->kind == PROC_TID ||
			 node->kind == PROC_SYS ||
			 node->kind == PROC_SYS_KERNEL;

	return kind == UK_FS_TMPL_DIR
		   ? directory
		   : node->kind == PROC_SELF || node->kind == PROC_EXE ||
			 node->kind == PROC_FD;
}

static struct proc_node *volume_open(union uk_fs_vopen_vol volume __unused,
				     unsigned long flags __unused,
				     union uk_fs_vopen_data data __unused,
				     size_t format __unused)
{
	return node_new(PROC_ROOT, NULL, NULL, -1, NULL, 0);
}

static ssize_t node_io(struct proc_node *node, const struct iovec *iov __unused,
		       size_t count __unused, size_t offset __unused,
		       long flags __unused, unsigned long mount_flags __unused)
{
	return node_kind(node, UK_FS_TMPL_DIR) ? -EISDIR : -EINVAL;
}

static bool io_has_data(const struct iovec *iov, size_t count)
{
	for (size_t i = 0; i < count; i++)
		if (iov[i].iov_len)
			return true;
	return false;
}

#if CONFIG_LIBUKVMEM
static int mapping_line(const struct uk_posix_vm_mapping *map, char *line,
			size_t capacity)
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

static ssize_t maps_read(struct uk_posix_vm_view *view, const struct iovec *iov,
			 size_t count, size_t offset)
{
	struct uk_posix_vm_mapping *maps;
	size_t length, vector = 0, position = 0, written = 0;
	const size_t capacity = 4 * PATH_MAX + 160;
	char *line;
	int rc;

	if (!io_has_data(iov, count))
		return 0;
	rc = uk_posix_vm_maps(view, &maps, &length);

	if (rc)
		return rc;
	if (!length) {
		uk_posix_vm_maps_release(maps, 0);
		return 0;
	}
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
	uk_posix_vm_maps_release(maps, length);
	return written ? (ssize_t)written : rc < 0 ? rc : 0;
}
#endif

static ssize_t comm_node_read(struct proc_node *node, const struct iovec *iov,
			      size_t count, size_t offset)
{
	char name[UK_POSIX_TASK_NAME_SIZE + 1];
	size_t length, written = 0;
	int rc = uk_posix_process_task_name(node->process, node->task, name);

	if (rc)
		return rc;
	length = strnlen(name, UK_POSIX_TASK_NAME_SIZE);
	name[length++] = '\n';
	for (size_t index = 0; index < count && offset < length; index++) {
		size_t bytes = MIN(length - offset, iov[index].iov_len);

		memcpy(iov[index].iov_base, name + offset, bytes);
		offset += bytes;
		written += bytes;
	}
	return written;
}

static ssize_t node_read(struct proc_node *node, const struct iovec *iov,
			 size_t count, size_t offset, long flags,
			 unsigned long mount_flags)
{
	if (node->kind == PROC_COMM || node->kind == PROC_TID_COMM)
		return comm_node_read(node, iov, count, offset);
#if CONFIG_LIBUKVMEM
	if (node->kind == PROC_MAPS) {
		struct uk_posix_vm_view *view;
		ssize_t result;

		if (!io_has_data(iov, count))
			return 0;
		view = uk_posix_process_task_vm_view_get(node->process,
							 node->task);
		if (PTRISERR(view))
			return PTR2ERR(view);
		result = maps_read(view, iov, count, offset);
		uk_posix_vm_view_put(view);
		return result;
	}
#endif
	return node_io(node, iov, count, offset, flags, mount_flags);
}

static ssize_t node_mem(struct proc_node *node __unused,
			enum uk_file_mem_op operation __unused,
			size_t offset __unused, size_t length __unused,
			struct iovec *iov __unused, size_t count __unused,
			unsigned long flags __unused)
{
	return -EOPNOTSUPP;
}

static struct uk_ofile *node_descriptor(struct proc_node *node)
{
	struct uk_fdtab *table =
	    uk_posix_process_task_fdtable(node->process, node->task);
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
	metadata->stx_mode =
	    node_kind(node, UK_FS_TMPL_DIR)
		? S_IFDIR | (node->kind == PROC_FDDIR ? 0500 : 0555)
		: S_IFLNK | 0777;
	metadata->stx_nlink = node_kind(node, UK_FS_TMPL_DIR) ? 2 : 1;
	metadata->stx_ino = node->inode;
	if (node->kind == PROC_FD) {
		unsigned int mode;

		uk_mutex_lock(&node_lock);
		mode = node->descriptor_mode;
		uk_mutex_unlock(&node_lock);
		metadata->stx_mode = S_IFLNK;
		if (!(mode & O_PATH)) {
			if ((mode & O_ACCMODE) != O_WRONLY)
				metadata->stx_mode |= 0500;
			if ((mode & O_ACCMODE) != O_RDONLY)
				metadata->stx_mode |= 0300;
		}
	}
	if (node->kind == PROC_SYS_SEM)
		metadata->stx_mode = S_IFREG | 0644;
	metadata->stx_blksize = 4096;
	if (node->kind == PROC_MAPS || node->kind == PROC_STATUS ||
	    node->kind == PROC_COMM || node->kind == PROC_TID_COMM)
		metadata->stx_mode =
		    S_IFREG |
		    (node->kind == PROC_MAPS || node->kind == PROC_STATUS
			 ? 0444
			 : 0644);
	return 0;
}

static int node_setstat(struct proc_node *node __unused,
			unsigned int mask __unused,
			const struct uk_statx *metadata __unused,
			unsigned long flags __unused)
{
	return -EROFS;
}

static int node_control(struct proc_node *node __unused, int family,
			int request, uintptr_t first __unused,
			uintptr_t second __unused, uintptr_t third __unused,
			unsigned long flags __unused)
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
		       struct proc_node **output,
		       union uk_fs_lookup_out *details __unused,
		       size_t *consumed)
{
	struct proc_node *found;
	const struct proc_fixed_entry *fixed;
	struct posix_process *process;
	struct uk_posix_task *task;
	pid_t pid = 0;
	size_t part = 0;

	if (node->task &&
	    !uk_posix_process_task_attached(node->process, node->task))
		return -ENOENT;
	if (node->process && !uk_posix_process_attached(node->process))
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
	} else if ((fixed = fixed_named(node->kind, name, part))) {
		found = node_new(fixed->kind, node, NULL, -1, NULL, 0);
	} else if ((node->kind == PROC_PID || node->kind == PROC_TID) &&
		   part == 3 && !memcmp(name, "exe", 3)) {
		found = node_new(PROC_EXE, node, NULL, -1, NULL, 0);
	} else if ((node->kind == PROC_PID || node->kind == PROC_TID) &&
		   part == 2 && !memcmp(name, "fd", 2)) {
		found = node_new(PROC_FDDIR, node, NULL, -1, NULL, 0);
#if CONFIG_LIBUKVMEM
	} else if ((node->kind == PROC_PID || node->kind == PROC_TID) &&
		   part == 4 && !memcmp(name, "maps", 4)) {
		found = node_new(PROC_MAPS, node, NULL, -1, NULL, 0);
#endif
	} else if (node->kind == PROC_PID && part == 4 &&
		   !memcmp(name, "task", 4)) {
		found = node_new(PROC_TASKDIR, node, NULL, -1, NULL, 0);
	} else if ((node->kind == PROC_PID || node->kind == PROC_TID) &&
		   ((part == 4 && !memcmp(name, "comm", 4)) ||
		    (part == 6 && !memcmp(name, "status", 6)))) {
		task = node->task;
		if (task)
			uk_posix_process_task_acquire(task);
		else
			task = uk_posix_process_task_get(node->process,
							 node->descriptor);
		if (!task)
			return -ENOENT;
		found = node_new(part == 6		  ? PROC_STATUS
				 : node->kind == PROC_PID ? PROC_COMM
							  : PROC_TID_COMM,
				 node, NULL, -1, task, 0);
	} else if (node->kind == PROC_TASKDIR) {
		if (name[0] == '0')
			return -ENOENT;
		pid = node_number(name, part);
		if (pid < 0)
			return pid;
		task = uk_posix_process_task_get(node->process, pid);
		if (!task)
			return -ENOENT;
		found = node_new(PROC_TID, node, NULL, pid, task, 0);
	} else if (node->kind == PROC_FDDIR) {
		struct uk_fdtab *table;
		struct uk_ofile *file;
		unsigned int descriptor_mode;

		if (part > 1 && name[0] == '0')
			return -ENOENT;
		pid = node_number(name, part);
		if (pid < 0)
			return pid;
		table =
		    uk_posix_process_task_fdtable(node->process, node->task);
		if (!table)
			return -ENOENT;
		file = uk_fdtab_get_from(table, pid);
		uk_fdtab_put(table);
		if (!file)
			return -ENOENT;
		descriptor_mode = uk_load_n(&file->mode);
		uk_ofile_release(file);
		found =
		    node_new(PROC_FD, node, NULL, pid, NULL, descriptor_mode);
	} else if (node->kind == PROC_ROOT) {
		if (name[0] == '0')
			return -ENOENT;
		pid = node_number(name, part);
		if (pid < 0)
			return pid;
		process = uk_posix_process_get(pid);
		if (!process)
			return -ENOENT;
		task = uk_posix_process_task_get(process, pid);
		if (!task) {
			uk_posix_process_put(process);
			return -ENOENT;
		}
		found = node_new(PROC_PID, node, process, pid, task, 0);
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
	struct proc_node *child[2];
	const char *name;
	char number[24];
	size_t next, length, record, consumed, used = 0;
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
	const size_t first_pid = 4;
#else
	const size_t first_pid = 3;
#endif
	pid_t pid;
	int rc;

	if ((node->process && !uk_posix_process_attached(node->process)) ||
	    (node->task &&
	     !uk_posix_process_task_attached(node->process, node->task)))
		return 0;
	for (;;) {
		if (*cursor >= INT_MAX)
			break;
		next = *cursor + 1;
		if (*cursor < 2) {
			name = *cursor ? ".." : ".";
		} else if (node->kind == PROC_FDDIR) {
			struct uk_fdtab *table;
			int descriptor;

			table = uk_posix_process_task_fdtable(node->process,
							      node->task);
			if (!table)
				break;
			descriptor = uk_fdtab_next(table, *cursor - 2);
			uk_fdtab_put(table);
			if (descriptor < 0)
				break;
			snprintf(number, sizeof(number), "%d", descriptor);
			name = number;
			next = (size_t)descriptor + 3;
		} else if (node->kind == PROC_PID || node->kind == PROC_TID) {
			static const char *const names[] = {
			    "exe",  "fd", "task", "comm", "status",
#if CONFIG_LIBUKVMEM
			    "maps",
#endif
			};

			size_t index = *cursor - 2;

			if (node->kind == PROC_TID && index >= 2)
				index++;
			if (index >= ARRAY_SIZE(names))
				break;
			name = names[index];
		} else if (node->kind == PROC_TASKDIR) {
			pid = uk_posix_process_next_tid(node->process,
							*cursor - 2);
			if (!pid)
				break;
			snprintf(number, sizeof(number), "%d", pid);
			name = number;
			next = (size_t)pid + 2;
		} else {
			const struct proc_fixed_entry *fixed =
				fixed_at(node->kind, *cursor - 2);

			if (fixed) {
				name = fixed->name;
			} else {
				if (node->kind != PROC_ROOT)
					break;
				pid = uk_posix_process_next_pid(*cursor -
								first_pid);
				if (!pid)
					break;
				snprintf(number, sizeof(number), "%d", pid);
				name = number;
				next = (size_t)pid + first_pid;
			}
		}
		length = strlen(name);
		record = UKFS_DIRENT_RECLEN(length);
		if (record > size - used)
			return used ? (ssize_t)used : -EINVAL;
		rc = node_lookup(node, name, length, child, NULL, &consumed);
		if (rc < 0) {
			if (rc != -ENOENT && rc != -ESRCH)
				return used ? (ssize_t)used : rc;
			*cursor = next;
			continue;
		}
		entry = (void *)((char *)buffer + used);
		memset(entry, 0, record);
		entry->d_ino = child[0]->inode;
		entry->d_off = next;
		entry->d_reclen = record;
		entry->d_type = node_kind(child[0], UK_FS_TMPL_DIR)   ? DT_DIR
				: node_kind(child[0], UK_FS_TMPL_SYM) ? DT_LNK
								      : DT_REG;
		memcpy(entry->d_name, name, length);
		node_release(child[0]);
		if (rc == UKFS_STOP_SYM)
			node_release(child[1]);
		used += record;
		*cursor = next;
	}
	return used;
}

static struct proc_node *
node_create(struct proc_node *node __unused, const char *name __unused,
	    size_t length __unused, unsigned int mode __unused,
	    int flags __unused,
	    union UK_FS_TMPL_LIVE_CREATE_TARGET(procfs) target __unused,
	    unsigned long mount_flags __unused)
{
	return ERR2PTR(-EROFS);
}

static int node_unlink(struct proc_node *node __unused,
		       const char *name __unused, size_t length __unused,
		       unsigned int flags __unused,
		       unsigned long mount_flags __unused)
{
	return -EROFS;
}

static int node_rename(struct proc_node *source __unused,
		       const char *name __unused, size_t length __unused,
		       struct proc_node *destination __unused,
		       const char *newname __unused, size_t newlength __unused,
		       unsigned int flags __unused,
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
		length =
		    snprintf(number, sizeof(number), "%d", uk_sys_getpid());
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
	path = uk_posix_process_task_executable(node->process, node->task);
	if (PTRISERR(path))
		return (struct uk_fs_path){.s = ERR2PTR(PTR2ERR(path))};
	name = uk_posix_vfs_path_name(path);
	uk_posix_vfs_path_release(path);
	return name;
}

static struct uk_vfs_path *node_follow(const struct uk_file *file,
				       enum uk_fs_link_policy policy);

static const struct uk_file *proc_open(const struct uk_file *inode,
				       unsigned int mode);

UK_FS_TMPL_LIVE_OPSTABLE(procfs, procfs_live_ops);
static const struct procfs_live_ops procfs_ops = {.live_vopen = volume_open,
						  .live_open = proc_open,
						  .live_read = node_read,
						  .live_write = node_io,
						  .live_mem = node_mem,
						  .live_getstat = node_stat,
						  .live_setstat = node_setstat,
						  .live_ctl = node_control,
						  .live_fs_stat = volume_stat,
						  .live_fs_sync = volume_sync,
						  .live_fs_lookup = node_lookup,
						  .live_fs_listdir = node_list,
						  .live_fs_create = node_create,
						  .live_fs_unlink = node_unlink,
						  .live_fs_rename = node_rename,
						  .live_fs_readlink =
						      node_readlink,
						  .live_nodekind = node_kind,
						  .live_state = node_state,
						  .live_errnode = node_error,
						  .live_acquire = node_acquire,
						  .live_release = node_release};

UK_FS_TMPL_LIVE_GENERATE_STATIC(procfs, struct proc_node *, node_compare,
				procfs_ops, UKFS_MODE_NOIOLOCK, node_follow,
				false);

static const char PROC_OPEN_VOLID[] = "proc_open";

struct proc_text_state {
	char *text;
	size_t capacity;
	size_t length;
	size_t next;
	bool valid;
};

struct proc_description {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_mutex lock;
	const struct uk_file *inode;
	union {
		struct proc_text_state text;
#if CONFIG_LIBUKVMEM
		struct uk_posix_vm_view *maps;
#endif
	} data;
};

static struct proc_description *proc_description(const struct uk_file *file)
{
	UK_ASSERT(file->vol == PROC_OPEN_VOLID);
	return file->node;
}

static int text_reserve(struct proc_text_state *text, size_t capacity)
{
	char *buffer;

	if (capacity <= text->capacity)
		return 0;
	buffer = uk_realloc(uk_alloc_get_default(), text->text, capacity);
	if (!buffer)
		return -ENOMEM;
	text->text = buffer;
	text->capacity = capacity;
	return 0;
}

static int __printf(2, 3)
text_append(struct proc_text_state *text, const char *format, ...)
{
	va_list args;
	int length;

	va_start(args, format);
	length = vsnprintf(text->text + text->length,
			   text->capacity - text->length, format, args);
	va_end(args);
	if (length < 0 || (size_t)length >= text->capacity - text->length)
		return -EOVERFLOW;
	text->length += length;
	return 0;
}

#if CONFIG_LIBPOSIX_USER
static int status_credentials_append(struct proc_text_state *text,
				     const struct uk_posix_task_status *status)
{
	const struct pu_credentials *credentials = status->credentials;
	const struct pu_identity *ids = pu_credentials_identity(credentials);
	const struct pu_privileges *caps =
	    pu_credentials_privileges(credentials);
	const __u32 *groups = pu_credentials_group_ids(credentials);
	size_t count = pu_credentials_group_count(credentials);
	int rc;

	rc = text_append(text,
			 "Uid:\t%u\t%u\t%u\t%u\n"
			 "Gid:\t%u\t%u\t%u\t%u\nGroups:\t",
			 ids->real_uid, ids->effective_uid, ids->saved_uid,
			 ids->fs_uid, ids->real_gid, ids->effective_gid,
			 ids->saved_gid, ids->fs_gid);
	if (rc)
		return rc;
	for (size_t i = 0; i < count; i++) {
		rc = text_append(text, "%u ", groups[i]);
		if (rc)
			return rc;
	}
	return text_append(text,
			   "\nThreads:\t%zu\n"
			   "CapInh:\t%016llx\nCapPrm:\t%016llx\n"
			   "CapEff:\t%016llx\nCapBnd:\t%016llx\n"
			   "CapAmb:\t%016llx\nNoNewPrivs:\t%u\n",
			   status->threads,
			   (unsigned long long)caps->inheritable,
			   (unsigned long long)caps->permitted,
			   (unsigned long long)caps->effective,
			   (unsigned long long)caps->bounding,
			   (unsigned long long)caps->ambient,
			   (unsigned int)status->no_new_privs);
}
#endif

static int status_snapshot(struct proc_description *description)
{
	static const char *const states[] = {
	    [UK_POSIX_TASK_RUNNABLE] = "R (running)",
	    [UK_POSIX_TASK_BLOCKED] = "S (sleeping)",
	    [UK_POSIX_TASK_ZOMBIE] = "Z (zombie)"};
	struct proc_text_state *text = &description->data.text;
	struct proc_node *node = procfs_FILE_NODE(description->inode);
	struct uk_posix_task_status status;
	char name[2 * UK_POSIX_TASK_NAME_SIZE + 1];
	size_t length = 0, groups = 0;
	int rc =
	    uk_posix_process_task_status(node->process, node->task, &status);

	if (rc)
		return rc;
#if CONFIG_LIBPOSIX_USER
	groups = pu_credentials_group_count(status.credentials);
#endif
	if (groups > (SSIZE_MAX - 1024) / 11) {
		rc = -EOVERFLOW;
		goto out;
	}
	rc = text_reserve(text, 1024 + groups * 11);
	if (rc)
		goto out;
	for (size_t i = 0; i < sizeof(status.name) && status.name[i]; i++) {
		char byte = status.name[i];

		if (byte == '\\' || byte == '\n')
			name[length++] = '\\';
		name[length++] = byte == '\n' ? 'n' : byte;
	}
	name[length] = 0;
	text->length = 0;
	rc = text_append(text,
			 "Name:\t%s\nState:\t%s\nTgid:\t%d\nPid:\t%d\n"
			 "PPid:\t%d\n",
			 name, states[status.state], status.tgid, status.pid,
			 status.ppid);
	if (rc)
		goto out;
#if CONFIG_LIBPOSIX_USER
	rc = status_credentials_append(text, &status);
#else
	rc = text_append(text, "Threads:\t%zu\n", status.threads);
#endif
	if (!rc)
		text->valid = true;
out:
	uk_posix_process_task_status_release(&status);
	return rc;
}

static int text_snapshot(struct proc_description *description)
{
	struct proc_text_state *state = &description->data.text;
	struct proc_node *node = procfs_FILE_NODE(description->inode);
	int error;

	state->valid = false;
#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
	if (node->kind == PROC_SYS_SEM) {
		struct uk_posix_sem_limits limits;

		error = text_reserve(state, 64);
		if (error)
			return error;
		uk_posix_sem_limits_get(&limits);
		state->length = 0;
		error = text_append(state, "%d\t%d\t%d\t%d\n", limits.per_set,
				    limits.total, limits.operations,
				    limits.sets);
		state->valid = !error;
		return error;
	}
#endif
	if (node->kind == PROC_STATUS)
		return status_snapshot(description);
	error = text_reserve(state, UK_POSIX_TASK_NAME_SIZE + 1);
	if (error)
		return error;
	error =
	    uk_posix_process_task_name(node->process, node->task, state->text);

	if (error)
		return error;
	state->length = strnlen(state->text, UK_POSIX_TASK_NAME_SIZE);
	state->text[state->length++] = '\n';
	state->valid = true;
	return 0;
}

static ssize_t text_read(const struct uk_file *file, const struct iovec *iov,
			 size_t count, size_t offset, long flags __unused)
{
	struct proc_description *description = proc_description(file);
	struct proc_text_state *state = &description->data.text;
	size_t written = 0, position = offset;
	int error = 0;

	if (offset &&
	    procfs_FILE_NODE(description->inode)->kind == PROC_SYS_SEM)
		return 0;
	if (!io_has_data(iov, count))
		return 0;
	uk_mutex_lock(&description->lock);
	if (!state->valid || !offset || offset != state->next) {
		error = text_snapshot(description);
		if (error)
			goto out;
	}
	for (size_t i = 0; i < count && position < state->length; i++) {
		size_t bytes = MIN(iov[i].iov_len, state->length - position);
		size_t copied = uk_nofault_memcpy(iov[i].iov_base,
					  state->text + position, bytes, 0);

		position += copied;
		written += copied;
		if (copied != bytes) {
			error = -EFAULT;
			break;
		}
	}
	state->next = offset + written;
out:
	uk_mutex_unlock(&description->lock);
	return written ? (ssize_t)written : error;
}

static struct uk_file_io_result
text_write(const struct uk_file *file, const struct iovec *iov, size_t count,
	   size_t offset, long flags __unused, enum uk_file_write_kind kind)
{
	struct proc_node *node =
	    procfs_FILE_NODE(proc_description(file)->inode);
	struct uk_file_io_result result = {.position = UK_FILE_WRITE_KEEP};

#if CONFIG_LIBPOSIX_PROCESS_SYSV_SEM
	if (node->kind == PROC_SYS_SEM)
		return uk_file_io_status(
			uk_posix_sem_limits_write(iov, count, offset));
#else
	(void)offset;
#endif
	if (node->kind == PROC_STATUS)
		return uk_file_io_status(-EINVAL);
	UK_ASSERT(kind == UK_FILE_WRITE_VECTOR || kind == UK_FILE_WRITE_SCALAR);
	UK_ASSERT(kind != UK_FILE_WRITE_SCALAR || count == 1);
	for (size_t i = 0; i < count; i++) {
		char name[UK_POSIX_TASK_NAME_SIZE] = {0};
		size_t length = MIN(iov[i].iov_len, sizeof(name) - 1);

		if (!iov[i].iov_len && kind == UK_FILE_WRITE_VECTOR)
			continue;
		if (uk_nofault_memcpy(name, iov[i].iov_base, length, 0) !=
		    length) {
			result.error = -EFAULT;
			break;
		}
		result.error = uk_posix_process_task_set_name(node->process,
							      node->task, name);
		if (result.error)
			break;
		result.count += iov[i].iov_len;
	}
	return result;
}

static off_t proc_seek_offset(off_t offset, int whence, off_t current)
{
	if (whence == SEEK_CUR) {
		if (offset > 0 && current > INT64_MAX - offset)
			return -EINVAL;
		offset += current;
	} else if (whence != SEEK_SET) {
		return -EINVAL;
	}
	if (offset < 0)
		return -EINVAL;
	return offset;
}

static off_t text_seek(const struct uk_file *file, off_t offset, int whence,
		       off_t current)
{
	struct proc_description *description = proc_description(file);
	int error = 0;

	offset = proc_seek_offset(offset, whence, current);
	if (offset < 0)
		return offset;
	uk_mutex_lock(&description->lock);
	if (offset != current) {
		if (offset)
			error = text_snapshot(description);
		else
			description->data.text.valid = false;
		if (!error)
			description->data.text.next = offset;
	}
	uk_mutex_unlock(&description->lock);
	return error ? error : offset;
}

static const struct uk_file *proc_file_open(const struct uk_file *file,
					    unsigned int mode)
{
	return proc_open(proc_description(file)->inode, mode);
}

static ssize_t proc_file_mem(const struct uk_file *file,
			     enum uk_file_mem_op operation, size_t offset,
			     size_t length, struct iovec *iov, size_t count)
{
	return uk_file_mem(proc_description(file)->inode, operation, offset,
			   length, iov, count);
}

static int proc_file_getstat(const struct uk_file *file, unsigned int mask,
			     struct uk_statx *status)
{
	return uk_file_getstat(proc_description(file)->inode, mask, status);
}

static int proc_file_setstat(const struct uk_file *file, unsigned int mask,
			     const struct uk_statx *status)
{
	return uk_file_setstat(proc_description(file)->inode, mask, status);
}

static int proc_file_ctl(const struct uk_file *file, int family, int request,
			 uintptr_t first, uintptr_t second, uintptr_t third)
{
	return uk_file_ctl(proc_description(file)->inode, family, request,
			   first, second, third);
}

static void proc_file_release(const struct uk_file *file, int what)
{
	struct proc_description *description = proc_description(file);

	if (what & UK_FILE_RELEASE_RES) {
#if CONFIG_LIBUKVMEM
		if (procfs_FILE_NODE(description->inode)->kind == PROC_MAPS)
			uk_posix_vm_view_put(description->data.maps);
#endif
		if (procfs_FILE_NODE(description->inode)->kind != PROC_MAPS)
			uk_free(uk_alloc_get_default(),
				description->data.text.text);
		uk_file_release(description->inode);
	}
	if (what & UK_FILE_RELEASE_OBJ)
		uk_free(uk_alloc_get_default(), description);
}

static const struct uk_file_ops text_ops = {.open = proc_file_open,
					    .read = text_read,
					    .write = text_write,
					    .seek = text_seek,
					    .mem = proc_file_mem,
					    .getstat = proc_file_getstat,
					    .setstat = proc_file_setstat,
					    .ctl = proc_file_ctl};

#if CONFIG_LIBUKVMEM
static ssize_t maps_description_read(const struct uk_file *file,
				     const struct iovec *iov, size_t count,
				     size_t offset, long flags __unused)
{
	return maps_read(proc_description(file)->data.maps, iov, count, offset);
}

static off_t maps_description_seek(const struct uk_file *file __unused,
				   off_t offset, int whence, off_t current)
{
	return proc_seek_offset(offset, whence, current);
}

static const struct uk_file_ops maps_ops = {.open = proc_file_open,
					    .read = maps_description_read,
					    .write = uk_file_anon_write,
					    .seek = maps_description_seek,
					    .mem = proc_file_mem,
					    .getstat = proc_file_getstat,
					    .setstat = proc_file_setstat,
					    .ctl = proc_file_ctl};
#endif

static const struct uk_file *proc_open(const struct uk_file *inode,
				       unsigned int mode)
{
	struct proc_node *node = procfs_FILE_NODE(inode);
	struct proc_description *description;
	const struct uk_file_ops *ops = &text_ops;
	bool maps = false;

#if CONFIG_LIBUKVMEM
	maps = node->kind == PROC_MAPS;
#endif

	if ((mode & O_PATH) ||
	    (!maps && node->kind != PROC_STATUS && node->kind != PROC_COMM &&
	     node->kind != PROC_TID_COMM && node->kind != PROC_SYS_SEM)) {
		uk_file_acquire(inode);
		return inode;
	}
	description = uk_zalloc(uk_alloc_get_default(), sizeof(*description));
	if (!description)
		return ERR2PTR(-ENOMEM);
#if CONFIG_LIBUKVMEM
	if (maps) {
		struct uk_posix_vm_view *view;

		view = uk_posix_process_task_vm_view_get(node->process,
							 node->task);
		description->data.maps = view;
		if (PTRISERR(description->data.maps)) {
			const struct uk_file *error =
			    ERR2PTR(PTR2ERR(description->data.maps));

			uk_free(uk_alloc_get_default(), description);
			return error;
		}
		ops = &maps_ops;
	}
#endif
	uk_file_acquire(inode);
	description->inode = inode;
	uk_mutex_init(&description->lock);
	description->references =
	    UK_FILE_REFCNT_INIT_VALUE(description->references);
	description->state = UK_FILE_STATE_EVENTS_INIT_VALUE(description->state,
							     POLLIN | POLLOUT);
	description->file = (struct uk_file){.vol = PROC_OPEN_VOLID,
					     .node = description,
					     .ops = ops,
					     .nofault_io = !maps,
					     .no_pwrite = true,
					     .refcnt = &description->references,
					     .state = &description->state,
					     ._release = proc_file_release};
	return &description->file;
}

static struct uk_vfs_path *node_follow(const struct uk_file *file,
				       enum uk_fs_link_policy policy)
{
	struct proc_node *node = procfs_FILE_NODE(file);
	struct uk_vfs_path *path;

	if (node->kind == PROC_SELF || node->kind == PROC_MAPS ||
	    node->kind == PROC_SYS_SEM || node->kind == PROC_STATUS ||
	    node->kind == PROC_COMM || node->kind == PROC_TID_COMM)
		return NULL;
	if (policy != UKFS_LINK_FOLLOW)
		return ERR2PTR(policy == UKFS_LINK_SCOPED ? -EXDEV : -ELOOP);
	if (node->kind == PROC_FD) {
		struct uk_ofile *descriptor = node_descriptor(node);

		if (!descriptor)
			return ERR2PTR(-ENOENT);
		path = uk_posix_vfs_ofile_path(descriptor);
		uk_ofile_release(descriptor);
		return path;
	}
	return uk_posix_process_task_executable(node->process, node->task);
}

UK_FS_DRIVER_REGISTER(proc, UK_FS_TMPL_LIVE_OP_VOPEN(procfs),
		      UK_FS_VOPEN_VOL_IGNORE | UK_FS_VOPEN_DATA_IGNORE);
