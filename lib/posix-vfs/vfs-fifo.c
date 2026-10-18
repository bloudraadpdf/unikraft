/* SPDX-License-Identifier: BSD-3-Clause */
#include <sys/stat.h>
#include <uk/alloc.h>
#include <uk/file/nops.h>
#include <uk/fs.h>
#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/posix-pipe.h>

#include "vfs-fifo.h"

struct fifo_session {
	struct uk_list_head link;
	const struct uk_file *inode;
	struct uk_pipefile *pipe;
	unsigned int reservations;
};

struct fifo_open {
	struct uk_file file;
	uk_file_refcnt references;
	const struct uk_file *inode;
	const struct uk_file *endpoint;
	struct fifo_session *session;
};

static UK_LIST_HEAD(sessions);
static struct uk_mutex sessions_lock = UK_MUTEX_INITIALIZER(sessions_lock);

static struct fifo_session *fifo_find(const struct uk_file *inode)
{
	struct fifo_session *session;

	uk_list_for_each_entry(session, &sessions, link)
		if (session->inode->state == inode->state)
			return session;
	return NULL;
}

static void fifo_session_free(struct fifo_session *session)
{
	uk_pipefile_release(session->pipe);
	uk_file_release(session->inode);
	uk_free(uk_alloc_get_default(), session);
}

static struct fifo_session *fifo_reserve(const struct uk_file *inode)
{
	struct fifo_session *session, *candidate;
	struct uk_alloc *allocator = uk_alloc_get_default();

	uk_mutex_lock(&sessions_lock);
	session = fifo_find(inode);
	if (session)
		session->reservations++;
	uk_mutex_unlock(&sessions_lock);
	if (session)
		return session;
	candidate = uk_malloc(allocator, sizeof(*candidate));
	if (!candidate)
		return NULL;
	candidate->pipe = uk_pipefile_alloc(UK_PIPEFILE_POSIX);
	if (!candidate->pipe) {
		uk_free(allocator, candidate);
		return NULL;
	}
	candidate->inode = inode;
	candidate->reservations = 1;
	uk_file_acquire(inode);
	uk_mutex_lock(&sessions_lock);
	session = fifo_find(inode);
	if (session) {
		session->reservations++;
	} else {
		uk_list_add_tail(&candidate->link, &sessions);
		session = candidate;
		candidate = NULL;
	}
	uk_mutex_unlock(&sessions_lock);
	if (candidate)
		fifo_session_free(candidate);
	return session;
}

static void fifo_unreserve(struct fifo_session *session)
{
	bool last;

	uk_mutex_lock(&sessions_lock);
	last = !--session->reservations;
	if (last)
		uk_list_del(&session->link);
	uk_mutex_unlock(&sessions_lock);
	if (last)
		fifo_session_free(session);
}

static struct fifo_open *fifo_description(const struct uk_file *file)
{
	return __containerof(file, struct fifo_open, file);
}

static const struct uk_file *fifo_inode(const struct uk_file *file)
{
	return fifo_description(file)->inode;
}

static bool fifo_lock_inode(const struct uk_file *inode)
{
	return !(uk_fs_mode_constraints(inode) & UKFS_MODE_NOIOLOCK);
}

static int fifo_inode_getstat(const struct uk_file *inode, unsigned int mask,
			      struct uk_statx *status)
{
	bool lock = fifo_lock_inode(inode);
	int result;

	if (lock)
		uk_file_rlock(inode);
	result = uk_file_getstat(inode, mask, status);
	if (lock)
		uk_file_runlock(inode);
	return result;
}

static int fifo_getstat(const struct uk_file *file, unsigned int mask,
			struct uk_statx *status)
{
	return fifo_inode_getstat(fifo_inode(file), mask, status);
}

static int fifo_setstat(const struct uk_file *file, unsigned int mask,
			const struct uk_statx *status)
{
	const struct uk_file *inode = fifo_inode(file);
	bool lock = fifo_lock_inode(inode);
	int result;

	if (lock)
		uk_file_wlock(inode);
	result = uk_file_setstat(inode, mask, status);
	if (lock)
		uk_file_wunlock(inode);
	return result;
}

static int fifo_touch(const struct uk_file *inode,
		      enum uk_file_touch operation, unsigned int flags)
{
	bool lock = fifo_lock_inode(inode);
	int result;

	if (lock)
		uk_file_wlock(inode);
	result = uk_file_touch(inode, operation, flags & O_NOATIME);
	if (lock)
		uk_file_wunlock(inode);
	return result;
}

static ssize_t fifo_read(const struct uk_file *file, const struct iovec *iov,
			 size_t count, size_t offset, long flags)
{
	ssize_t result = uk_file_read(fifo_description(file)->endpoint, iov,
				      count, offset, flags);

	if (result > 0)
		(void)fifo_touch(fifo_inode(file), UK_FILE_TOUCH_READ, flags);
	return result;
}

static struct uk_file_io_result
fifo_write(const struct uk_file *file, const struct iovec *iov, size_t count,
	   size_t offset, long flags, enum uk_file_write_kind kind)
{
	struct uk_file_io_result result =
		uk_file_write(fifo_description(file)->endpoint, iov,
			      count, offset, flags, kind);

	if (result.count) {
		int error =
		    fifo_touch(fifo_inode(file), UK_FILE_TOUCH_WRITE, flags);

		if (error)
			result.error = error;
	}
	return result;
}

static int fifo_ctl(const struct uk_file *file, int family, int request,
		    uintptr_t first, uintptr_t second, uintptr_t third)
{
	return uk_file_ctl(fifo_description(file)->endpoint, family, request,
			   first, second, third);
}

static int fifo_lookup(const struct uk_file *file, const char *path,
		       size_t length, unsigned int flags,
		       union uk_fs_lookup_out *output, size_t *consumed)
{
	return uk_fs_lookupat(fifo_inode(file), path, length, flags, output,
			      consumed);
}

static int fifo_mount(const struct uk_file *file, const struct uk_file **target)
{
	return uk_fs_mountat(fifo_inode(file), target);
}

static const struct uk_file *fifo_rebind(const struct uk_file *file,
					 unsigned long flags, const void *data)
{
	return uk_fs_rebind(fifo_inode(file), flags, data);
}

static int fifo_statfs(const struct uk_file *file, struct statfs *status)
{
	return uk_fs_stat(fifo_inode(file), status);
}

static int fifo_syncfs(const struct uk_file *file)
{
	return uk_fs_sync(fifo_inode(file));
}

static const struct uk_file_ops fifo_ops = {
	.read = fifo_read,
	.write = fifo_write,
	.mem = uk_file_nop_mem,
	.getstat = fifo_getstat,
	.setstat = fifo_setstat,
	.ctl = fifo_ctl
};

static const struct uk_fs_ops fifo_fsops = {
	.inode = fifo_inode,
	.lookup = fifo_lookup,
	.mount = fifo_mount,
	.rebind = fifo_rebind,
	.stat = fifo_statfs,
	.sync = fifo_syncfs,
	.constraints = UKFS_MODE_NOSEEK | UKFS_MODE_NOIOLOCK
};

static void fifo_release(const struct uk_file *file, int what)
{
	struct fifo_open *opened = fifo_description(file);

	if (what & UK_FILE_RELEASE_RES) {
		uk_file_release(opened->endpoint);
		fifo_unreserve(opened->session);
	}
	if (what & UK_FILE_RELEASE_OBJ) {
		uk_file_release_weak(opened->endpoint);
		uk_file_release(opened->inode);
		uk_free(uk_alloc_get_default(), opened);
	}
}

const struct uk_file *vfs_fifo_open(const struct uk_file *inode,
				    unsigned int mode)
{
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct fifo_session *session;
	struct fifo_open *opened;
	const struct uk_file *endpoint;
	struct uk_statx status;
	int result;

	inode = uk_fs_inode(inode);
	if (!uk_fs_isfs(inode))
		return NULL;
	result = fifo_inode_getstat(inode, UK_STATX_TYPE, &status);
	if (result)
		return ERR2PTR(result);
	if ((status.stx_mode & S_IFMT) != S_IFIFO)
		return NULL;
	opened = uk_malloc(allocator, sizeof(*opened));
	if (!opened)
		return ERR2PTR(-ENOMEM);
	session = fifo_reserve(inode);
	if (!session) {
		uk_free(allocator, opened);
		return ERR2PTR(-ENOMEM);
	}
	endpoint = uk_pipefile_open_named(session->pipe, mode);
	if (PTRISERR(endpoint)) {
		fifo_unreserve(session);
		uk_free(allocator, opened);
		return endpoint;
	}
	opened->references = UK_FILE_REFCNT_INIT_VALUE(opened->references);
	opened->inode = inode;
	opened->endpoint = endpoint;
	opened->session = session;
	uk_file_acquire(inode);
	uk_file_acquire_weak(endpoint);
	opened->file = (struct uk_file){
		.vol = inode->vol,
		.node = inode->node,
		.ops = &fifo_ops,
		.write_sigpipe = endpoint->write_sigpipe,
		.fsops = &fifo_fsops,
		.refcnt = &opened->references,
		.state = endpoint->state,
		._release = fifo_release
	};
	return &opened->file;
}
