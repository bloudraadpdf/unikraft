/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */
#define _GNU_SOURCE

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/time.h>
#include <sys/vfs.h>

#include <uk/alloc.h>
#include <uk/atomic.h>
#include <uk/errptr.h>
#include <uk/essentials.h>
#include <uk/fs.h>
#include <uk/fs/driver.h>
#include <uk/fs/pathutil.h>
#include <uk/mutex.h>
#include <uk/nofault.h>
#include <uk/posix-fdio.h>
#include <uk/posix-vfs.h>
#include <uk/vfs-open.h>
#include <uk/posix-vfsroot.h>
#include <uk/posix-fdtab.h>
#include "vfs-path.h"
#include "vfs-fifo.h"
#include <uk/spinlock.h>

#if CONFIG_LIBPOSIX_VFS_MULTICTX
#include <uk/init.h>
#include <uk/refcount.h>
#include <uk/thread.h>
#endif /* CONFIG_LIBPOSIX_VFS_MULTICTX */

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#if CONFIG_LIBPOSIX_VFS_MOUNT_CLEANUP
#include <uk/init.h>
#endif /* CONFIG_LIBPOSIX_VFS_MOUNT_CLEANUP */

/* posix-ey perms checking */
#if CONFIG_LIBPOSIX_VFS_PERMISSIONS
int uk_fs_checkperm(int desired __unused, int mode __unused,
		    int uid __unused, int gid __unused)
{
	/* TODO: impl & integrate w/ posix-user */
	return 0;
}
#endif /* CONFIG_LIBPOSIX_VFS_PERMISSIONS */

static struct vfs_mount vfs_initial_mount = {
	.root = &uk_posix_vfsroot_file,
	.references = UK_REFCOUNT_INITIALIZER(2),
	.attached = true
};

/* mount table */
struct vfs_mtab {
	struct uk_mutex lock;
	size_t top;
	struct vfs_mount *tab[CONFIG_LIBPOSIX_VFS_MAX_MOUNTS];
};

static struct vfs_mtab mtab = {
	.lock = UK_MUTEX_INITIALIZER(mtab.lock),
	.top = 0
};

#if CONFIG_LIBPOSIX_VFS_MOUNT_CLEANUP

static
void vfs_mtab_cleanup(struct uk_term_ctx *tctx __unused)
{
	/* This function is called at system teardown and is the only one
	 * touching the mtab; we can safely ignore any locks.
	 */
	for (size_t i = mtab.top; i-- > 0;) {
		const struct uk_file *point = mtab.tab[i]->point;
		const struct uk_file *prev = NULL;
		int r = uk_fs_mountat(point, &prev);

		if (unlikely(r)) {
			uk_pr_warn("Unable to unmount %p: %d\n", point, r);
		} else {
			uk_file_release(prev);
			mtab.tab[i]->attached = false;
			vfs_mount_release(mtab.tab[i]);
		}
	}
}

uk_rootfs_initcall(0x0, vfs_mtab_cleanup);

#endif /* CONFIG_LIBPOSIX_VFS_MOUNT_CLEANUP */

static struct vfs_mount *vfs_mounted_root(const struct uk_file *file)
{
	struct vfs_mount *mount = NULL;

	uk_mutex_lock(&mtab.lock);
	for (size_t i = mtab.top; i-- > 0;)
		if (mtab.tab[i]->root == file) {
			mount = mtab.tab[i];
			break;
		}
	uk_mutex_unlock(&mtab.lock);
	return mount;
}

/* per-process vfs state */

struct vfs_proc_state {
	const struct uk_file *fsroot;
	struct vfs_mount *root_mount;
	struct uk_ofile *cwd;
	struct uk_spinlock fsroot_lock;
	struct uk_spinlock cwd_lock;
	mode_t umask;
#if CONFIG_LIBPOSIX_VFS_MULTICTX
	__atomic refcnt;
#endif /* CONFIG_LIBPOSIX_VFS_MULTICTX */
};

static struct vfs_proc_state vfs_init_ctx = {
	.fsroot = &uk_posix_vfsroot_file,
	.root_mount = &vfs_initial_mount,
	.cwd = &uk_posix_vfsroot_ofile,
	.fsroot_lock = UK_SPINLOCK_INITIALIZER(),
	.cwd_lock = UK_SPINLOCK_INITIALIZER(),
	.umask = 0,
#if CONFIG_LIBPOSIX_VFS_MULTICTX
	.refcnt = UK_REFCOUNT_INITIALIZER(1)
#endif /* CONFIG_LIBPOSIX_VFS_MULTICTX */
};

#if CONFIG_LIBPOSIX_VFS_MULTICTX

/* When using multi-vfsctx, the init thread has a ref to the init vfs ctx.
 *
 * Newly-created raw threads will start off with a copy of their parent's ref,
 * as a compatibility stop-gap.
 * It is the responsibility of other posix libs and their callbacks to more
 * meaningfully init a new thread's vfs ctx, unsharing as necessary.
 */

/* Every thread keeps a counted reference to a VFS context */
static __uk_tls struct vfs_proc_state *vfs_ctx;

static inline
void vfs_ctx_acquire(struct vfs_proc_state *ctx)
{
	uk_refcount_acquire(&ctx->refcnt);
}

static inline
void vfs_ctx_release(struct vfs_proc_state *ctx)
{
	if (uk_refcount_release(&ctx->refcnt)) {
		uk_file_release(ctx->fsroot);
		vfs_mount_release(ctx->root_mount);
		uk_ofile_release(ctx->cwd);
		uk_free(uk_alloc_get_default(), ctx);
	}
}

static
int init_posix_vfs(struct uk_init_ctx *ictx __unused)
{
	vfs_ctx_acquire(&vfs_init_ctx);
	vfs_ctx = &vfs_init_ctx;
	return 0;
}

uk_lib_initcall(init_posix_vfs, 0x0);

static
int vfs_thread_init(struct uk_thread *child, struct uk_thread *parent)
{
	struct vfs_proc_state *ctx;

	if (!parent)
		ctx = &vfs_init_ctx;
	else
		ctx = uk_thread_uktls_var(parent, vfs_ctx);
	vfs_ctx_acquire(ctx);
	uk_thread_uktls_var(child, vfs_ctx) = ctx;
	return 0;
}

static
void vfs_thread_term(struct uk_thread *child)
{
	struct vfs_proc_state *ctx = uk_thread_uktls_var(child, vfs_ctx);

	if (ctx)
		vfs_ctx_release(ctx);
}

UK_THREAD_INIT(vfs_thread_init, vfs_thread_term);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING

static
struct vfs_proc_state *vfs_ctx_duplicate(struct vfs_proc_state *ctx)
{
	struct vfs_proc_state *newctx = uk_malloc(uk_alloc_get_default(),
						  sizeof(*newctx));

	if (newctx) {
		const struct uk_file *fsroot;
		struct uk_ofile *cwd;

		uk_spin_init(&newctx->fsroot_lock);
		uk_spin_init(&newctx->cwd_lock);
		newctx->refcnt = UK_REFCOUNT_INIT_VALUE(1);
		newctx->umask = ctx->umask;

		uk_spin_lock(&ctx->fsroot_lock);
		uk_spin_lock(&ctx->cwd_lock);
		fsroot = ctx->fsroot;
		newctx->root_mount = ctx->root_mount;
		vfs_mount_acquire(newctx->root_mount);
		cwd = ctx->cwd;
		uk_file_acquire(fsroot);
		uk_ofile_acquire(cwd);
		uk_spin_unlock(&ctx->cwd_lock);
		uk_spin_unlock(&ctx->fsroot_lock);

		newctx->fsroot = fsroot;
		newctx->cwd = cwd;
	}
	return newctx;
}

static
int vfs_clone(void *arg)
{
	struct posix_process_clone_event_data *event_data;
	const struct clone_args *cl_args;
	struct vfs_proc_state *newctx;
	struct vfs_proc_state *ctx;
	struct uk_thread *parent;
	struct uk_thread *child;

	event_data = (struct posix_process_clone_event_data *)arg;
	UK_ASSERT(event_data);

	cl_args = event_data->cl_args;
	UK_ASSERT(cl_args);

	child = event_data->child;
	parent = event_data->parent;

	if (!parent)
		return UK_EVENT_HANDLED_CONT;

	ctx = uk_thread_uktls_var(parent, vfs_ctx);
	UK_ASSERT(ctx); /* Do not call clone from raw threads */
	if ((cl_args->flags & CLONE_FS)) {
		/* Inherit parent's vfs ctx */
		/* As a compat stop-gap, the raw thread already inherited the
		 * parent's vfs ctx; we don't need to do anything.
		 *
		 * TODO: move inheritance here once stopgap is removed.
		 */
		UK_ASSERT(uk_thread_uktls_var(child, vfs_ctx) == ctx);
		return UK_EVENT_HANDLED_CONT;
	} else {
		/* Duplicate parent's vfs ctx */
		newctx = vfs_ctx_duplicate(ctx);
		if (unlikely(!newctx))
			return -ENOMEM;
		/* Compat stop-gap: release previous duplicate ref */
		UK_ASSERT(uk_thread_uktls_var(child, vfs_ctx) == ctx);
		vfs_ctx_release(ctx);
	}
	uk_thread_uktls_var(child, vfs_ctx) = newctx;
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_CLONE_HANDLER(CLONE_FS, vfs_clone);

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */

#else /* !CONFIG_LIBPOSIX_VFS_MULTICTX */

/* All threads share the same static reference */
static struct vfs_proc_state *const vfs_ctx = &vfs_init_ctx;

#endif /* !CONFIG_LIBPOSIX_VFS_MULTICTX */

static inline
const struct uk_file *vfs_active_fsroot(void)
{
	const struct uk_file *fsroot;

	UK_ASSERT(vfs_ctx);
	uk_spin_lock(&vfs_ctx->fsroot_lock);
	fsroot = vfs_ctx->fsroot;
	UK_ASSERT(fsroot);
	uk_file_acquire(fsroot);
	uk_spin_unlock(&vfs_ctx->fsroot_lock);
	return fsroot;
}

static void vfs_set_fsroot(struct uk_vfs_path *path)
{
	const struct uk_file *prev;
	struct vfs_mount *mount;

	uk_file_acquire(path->file);
	vfs_mount_acquire(path->mount);
	uk_spin_lock(&vfs_ctx->fsroot_lock);
	prev = uk_exchange_n(&vfs_ctx->fsroot, path->file);
	mount = uk_exchange_n(&vfs_ctx->root_mount, path->mount);
	uk_spin_unlock(&vfs_ctx->fsroot_lock);
	uk_file_release(prev);
	vfs_mount_release(mount);
}

static inline
struct uk_ofile *vfs_active_cwd(void)
{
	struct uk_ofile *cwd;

	UK_ASSERT(vfs_ctx);
	uk_spin_lock(&vfs_ctx->cwd_lock);
	cwd = vfs_ctx->cwd;
	UK_ASSERT(cwd);
	uk_ofile_acquire(cwd);
	uk_spin_unlock(&vfs_ctx->cwd_lock);
	return cwd;
}

static
struct uk_ofile *vfs_set_cwd(struct uk_ofile *newcwd)
{
	struct uk_ofile *prev;

	UK_ASSERT(vfs_ctx);
	UK_ASSERT(newcwd);
	uk_spin_lock(&vfs_ctx->cwd_lock);
	prev = uk_exchange_n(&vfs_ctx->cwd, newcwd);
	uk_spin_unlock(&vfs_ctx->cwd_lock);
	UK_ASSERT(prev);
	return prev;
}

static inline
mode_t vfs_apply_umask(mode_t mask)
{
	mode_t ubits = mask & 0777;

	ubits &= ~vfs_ctx->umask;
	return (mask & ~0777) | ubits;
}

/* path utils */

/**
 * INTERNAL. Retrieve the appropriate root for an *at lookup.
 *
 * @return
 *  - active filesystem root for absolute paths, otherwise
 *  - `f` if provided, otherwise
 *  - active current working directory
 */
static inline
const struct uk_file *vfs_atroot(const struct uk_file *f,
				 const char *path, size_t len)
{
	struct uk_ofile *cwd;
	const struct uk_file *ret;

	if (len >= 1 && path[0] == '/')
		return vfs_active_fsroot();
	if (f) {
		uk_file_acquire(f);
		return f;
	}
	cwd = vfs_active_cwd();
	ret = cwd->file;
	uk_file_acquire(ret);
	uk_ofile_release(cwd);
	return ret;
}

/**
 * INTERNAL. Retrieve the appropriate root for an *at lookup, along with a named
 * open file reference of the relative lookup base.
 *
 * @return
 *  - active filesystem root for absolute paths, otherwise
 *  - `of->file` if provided, otherwise
 *  - active current working directory
 */
static inline
const struct uk_file *vfs_atroot_named(struct uk_ofile *of,
				       const char *path, size_t len,
				       struct uk_ofile **base)
{
	struct uk_ofile *cwd;
	const struct uk_file *ret;

	if (len >= 1 && path[0] == '/')
		return vfs_active_fsroot();
	if (of) {
		uk_ofile_acquire(of);
		uk_file_acquire(of->file);
		*base = of;
		return of->file;
	}
	cwd = vfs_active_cwd();
	ret = cwd->file;
	uk_file_acquire(ret);
	*base = cwd;
	return ret;
}

static inline
int _vfs_should_lock(const struct uk_file *f)
{
	UK_ASSERT(uk_fs_isfs(f));
	return !(f->fsops->constraints & UKFS_MODE_NOIOLOCK);
}

static inline
void vfs_rlock(const struct uk_file *f)
{
	if (_vfs_should_lock(f))
		uk_file_rlock(f);
}

static inline
void vfs_runlock(const struct uk_file *f)
{
	if (_vfs_should_lock(f))
		uk_file_runlock(f);
}

static inline
void vfs_wlock(const struct uk_file *f)
{
	if (_vfs_should_lock(f))
		uk_file_wlock(f);
}

static inline
void vfs_wunlock(const struct uk_file *f)
{
	if (_vfs_should_lock(f))
		uk_file_wunlock(f);
}

#define VFS_LOOKUP_NOFOLLOW 1
#define VFS_LOOKUP_NO_XDEV 2
#define VFS_LOOKUP_CACHED 4
#define VFS_LOOKUP_IGNMNT_START 8

/**
 * INTERNAL. Symlink follow function.
 */
static
const struct uk_file *_vfs_lookupat(const struct uk_file *atroot,
				    const char *path, size_t len,
				    unsigned int flags,
				    struct vfs_mount **mount,
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
				    size_t lvl);
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
				    size_t *nsyms);
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM */

static inline
const struct uk_file *_vfs_symfollow(const struct uk_file *sym,
				     const struct uk_file *parent,
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
				     size_t lvl)
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
				     size_t *nsyms)
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM */
{
	struct uk_fs_path sympath;
	const struct uk_file *target;

#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
	if (unlikely(++lvl > CONFIG_LIBPOSIX_VFS_MAX_SYMDEPTH))
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
	if (unlikely(++*nsyms > CONFIG_LIBPOSIX_VFS_MAX_SYMNUM))
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM */
		return ERR2PTR(-ELOOP);

	if (sym->fsops->follow_link) {
		struct uk_vfs_path *follow =
			sym->fsops->follow_link(sym, UKFS_LINK_FOLLOW);

		if (PTRISERR(follow))
			return ERR2PTR(PTR2ERR(follow));
		if (follow) {
			target = uk_posix_vfs_path_file(follow);
			uk_posix_vfs_path_release(follow);
			return target;
		}
	}

	sympath = uk_fs_readlink(sym);
	if (unlikely(PTRISERR(sympath.s)))
		return ERR2PTR(PTR2ERR(sympath.s));
	target = _vfs_lookupat(vfs_atroot(parent, sympath.s, sympath.len),
			       sympath.s, sympath.len, 0, NULL,
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
			       lvl);
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
			       nsyms);
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM */
	uk_fs_path_release(sympath);
	return target;
}


/**
 * INTERNAL. Main lookup function.
 *
 * Uses `lvl`/`nsyms` to enforce a limit on symlinks traversed.
 * Consumes the `atroot` file reference.
 */
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
static
const struct uk_file *_vfs_lookupat(const struct uk_file *atroot,
				    const char *path, size_t len,
				    unsigned int flags,
				    struct vfs_mount **mount,
				    size_t lvl)
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
static
const struct uk_file *_vfs_lookupat(const struct uk_file *atroot,
				    const char *path, size_t len,
				    unsigned int flags,
				    struct vfs_mount **mount,
				    size_t *nsyms)
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM */
{
	const struct uk_file *d = atroot;
	const struct uk_file *fsroot;
	const struct uk_file *ret;
	unsigned int lflags =
	    UKFS_LOOKUP_NO_MNTAUX |
	    ((flags & VFS_LOOKUP_IGNMNT_START) ? UKFS_LOOKUP_IGNMNT_START : 0) |
	    ((flags & VFS_LOOKUP_CACHED) ? UKFS_LOOKUP_CACHED : 0);
	size_t pos;

	/* Skip leading '/' */
	while (len && path[0] == '/') {
		path++;
		len--;
	}
	/* vfs_active_fsroot() (& using locks) is only needed for a counted ref.
	 * We only need the pointer value for comparison; can simply read value.
	 */
	fsroot = vfs_ctx->fsroot;

	for (;;) {
		union uk_fs_lookup_out lout;
		const unsigned int lookup_flags = lflags;
		int r;

		UK_ASSERT(d);
		UK_ASSERT(uk_fs_isfs(d));
		/* Skip parent lookups if at fsroot */
		if (d == fsroot && uk_fs_path_isdotdot(path, len)) {
			lflags &= ~UKFS_LOOKUP_IGNMNT_START;
			path += (len == 2) ? 2 : 3;
			len -= (len == 2) ? 2 : 3;
			continue;
		}
		/* Lookup in filesystem */
		vfs_rlock(d);
		r = uk_fs_lookupat(d, path, len, lflags,
				   &lout, &pos);
		vfs_runlock(d);
		lflags = UKFS_LOOKUP_NO_MNTAUX |
			((flags & VFS_LOOKUP_CACHED) ? UKFS_LOOKUP_CACHED : 0);
		if (r < 0) {
			ret = ERR2PTR(r);
			goto out;
		}

		switch (r) {
		case UKFS_SUCCESS:
			ret = lout.target;
			goto out;

		case UKFS_STOP_END:
			UK_ASSERT(pos < len);
			ret = ERR2PTR(-ENOTDIR);
			goto out;

		case UKFS_STOP_NOD:
			if (!pos) {
				lflags |=
				    lookup_flags & UKFS_LOOKUP_IGNMNT_START;
				/* No progress, should wait */
				UK_ASSERT(lout.target == d);
				uk_file_release(lout.target);
				if (flags & VFS_LOOKUP_CACHED) {
					ret = ERR2PTR(-EAGAIN);
					goto out;
				}
				uk_file_poll(d, UKFD_POLLIN);
			} else {
				/* Progress made, continue */
				uk_file_release(d);
				d = lout.target;
			}
			break;

		case UKFS_STOP_MNT:
			if (flags & VFS_LOOKUP_NO_XDEV) {
				uk_file_release(lout.target);
				ret = ERR2PTR(-EXDEV);
				goto out;
			}
			if (mount) {
				struct vfs_mount *next =
				    vfs_mounted_root(lout.target);

				if (!next && *mount)
					next = (*mount)->parent;
				if (!next) {
					uk_file_release(lout.target);
					ret = ERR2PTR(-ESTALE);
					goto out;
				}
				*mount = next;
			}
			uk_file_release(d);
			d = lout.target;
			if (uk_fs_path_isdotdot(path + pos, len - pos))
				lflags |= UKFS_LOOKUP_IGNMNT_START;
			break;

		case UKFS_STOP_SYM:
			if ((flags & VFS_LOOKUP_NOFOLLOW) && pos == len) {
				/* No follow; return raw symlink */
				uk_file_release(lout.aux);
				ret = lout.target;
				goto out;
			}
			/* (Attempt to) follow symlink */
			ret = _vfs_symfollow(lout.target, lout.aux,
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
					     nsyms
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
					     lvl
#endif /* CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH */
			);

			uk_file_release(lout.target);
			uk_file_release(lout.aux);
			if (unlikely(PTRISERR(ret)))
				goto out;

			/* If symlink target is non-FS leaf, return directly */
			if (!uk_fs_isfs(ret) && pos == len)
				goto out;

			uk_file_release(d);

			d = ret;
			break;

		case UKFS_STOP_FILE:
			if (pos == len) {
				/* Return adopted file verbatim */
				ret = lout.target;
			} else if (uk_fs_isfs(lout.target)) {
				/* Adopted file is FS-aware; continue lookup */
				uk_file_release(d);
				d = lout.target;
				break;
			} else {
				/* Deeper lookups not possible */
				uk_file_release(lout.target);
				ret = ERR2PTR(-ENOTDIR);
			}
			goto out;

		case UKFS_STOP_SPEC:
			if (pos == len) {
				uk_pr_warn("Special files not supported\n");
				ret = ERR2PTR(-ENXIO);
			} else {
				/* Deeper lookups not possible */
				uk_file_release(lout.target);
				ret = ERR2PTR(-ENOTDIR);
			}
			goto out;

		default:
			UK_CRASH("Invalid return from fs lookup\n");
		}

		UK_ASSERT(pos <= len);
		path += pos;
		len -= pos;
	}
out:
	uk_file_release(d);
	return ret;
}

/**
 * INTERNAL. Main lookup function entry point.
 *
 * Convenience wrapper to start a lookup at symlink depth 0.
 * Consumes the `atroot` reference.
 */
static inline const struct uk_file *
vfs_lookupat_mount(const struct uk_file *atroot, const char *path, size_t len,
		   unsigned int flags, struct vfs_mount **mount)
{
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_NUM
	size_t nsyms = 0;

	return _vfs_lookupat(atroot, path, len, flags, mount, &nsyms);
#elif CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
	return _vfs_lookupat(atroot, path, len, flags, mount, 0);
#endif
}

static inline const struct uk_file *vfs_lookupat(const struct uk_file *atroot,
						 const char *path, size_t len,
						 unsigned int flags)
{
	return vfs_lookupat_mount(atroot, path, len, flags, NULL);
}

/* Convenience for the common call vfs_lookupat(vfs_atroot(...), ...) */
static inline
const struct uk_file *vfs_lookup(const struct uk_file *start,
				 const char *path, size_t len,
				 unsigned int flags)
{
	return vfs_lookupat(vfs_atroot(start, path, len), path, len, flags);
}

#if CONFIG_LIBPOSIX_VFS_PERMISSIONS

#define VFS_MODEMASK (UK_STATX_TYPE | UK_STATX_MODE | \
		      UK_STATX_UID | UK_STATX_GID)

/**
 * INTERNAL. Check whether the filesystem permissions of `f` allow the `desired`
 * access type.
 *
 * @return
 *  == 0: OK
 *   < 0: Negative errno.
 */
static
int vfs_check_perms(const struct uk_file *f, mode_t desired)
{
	struct uk_statx sx;
	int r = uk_file_getstat(f, VFS_MODEMASK, &sx);

	if (!r) {
		UK_ASSERT((sx.stx_mask & VFS_MODEMASK) == VFS_MODEMASK);
		r = uk_fs_checkperm(desired, sx.stx_mode,
				    sx.stx_uid, sx.stx_gid);
	}
	return r;
}

#else /* !CONFIG_LIBPOSIX_VFS_PERMISSIONS */

static inline
int vfs_check_perms(const struct uk_file *f __unused, mode_t desired __unused)
{
	return 0;
}

#endif /* !CONFIG_LIBPOSIX_VFS_PERMISSIONS */

static int vfs_check_access(const struct uk_file *file, mode_t desired)
{
	if (desired & X_OK) {
		struct uk_statx status;
		const unsigned int mask = UK_STATX_TYPE | UK_STATX_MODE;
		const bool lock = !uk_fs_isfs(file) || _vfs_should_lock(file);
		int result;

		if (lock)
			uk_file_rlock(file);
		result = uk_file_getstat(file, mask, &status);
		if (lock)
			uk_file_runlock(file);
		if (result)
			return result;
		UK_ASSERT((status.stx_mask & mask) == mask);
		if (S_ISREG(status.stx_mode) && !(status.stx_mode & 0111))
			return -EACCES;
	}
	return vfs_check_perms(file, desired);
}

/**
 * INTERNAL. Check whether `path` is acceptable for an *at lookup.
 *
 * NULL paths are always rejected.
 * Empty paths are only accepted if AT_EMPTY_PATH is set in `flags`.
 *
 * @return
 *  == 0: OK
 *   < 0: Negative errno.
 */
static inline
int vfs_check_path(const char *path, int flags)
{
	if (!path)
		return -EFAULT;
	if (strnlen(path, PATH_MAX) == PATH_MAX)
		return -ENAMETOOLONG;
	if (!path[0] && !(flags & AT_EMPTY_PATH))
		return -ENOENT;
	return 0;
}

/**
 * INTERNAL. Initialize in `of` a temporary open file description of `f` with
 * the open file mode `mode`, while also handling filesystem mode constraints.
 */
static inline
void vfs_tmpofile_init(struct uk_ofile *of, const struct uk_file *f,
		       unsigned int mode)
{
	if (uk_fs_isfs(f))
		mode |= uk_fs_mode_constraints(f);
	uk_ofile_init(of, f, mode);
}

/**
 * INTERNAL. Create a named open file description of `f` with the open file mode
 * `mode`, opened through `path` of length `pathlen` relative to open directory
 * `basedir`, while also handling filesystem mode constraints.
 */
static void vfs_ofile_destroy(struct uk_ofile *of)
{
	uk_posix_vfs_path_release(of->context);
	uk_free(uk_alloc_get_default(), of);
}

struct uk_fs_path uk_posix_vfs_ofile_name(struct uk_ofile *file)
{
	struct uk_statx metadata = {0};
	const char *name;
	char target[64];
	int length;

	if (file->destroy == vfs_ofile_destroy &&
	    file->context)
		return uk_posix_vfs_path_name(file->context);
	if (!uk_file_getstat(file->file, UK_STATX_TYPE | UK_STATX_INO,
			     &metadata) &&
	    (S_ISFIFO(metadata.stx_mode) || S_ISSOCK(metadata.stx_mode))) {
		length = snprintf(target, sizeof(target), "%s:[%llu]",
				  S_ISFIFO(metadata.stx_mode) ? "pipe" : "socket",
				  (unsigned long long)metadata.stx_ino);
		return uk_fs_path_copy(target, length, uk_alloc_get_default());
	}
	name = uk_ofile_name(file, "anon_inode");
	return uk_fs_path_copy(name, strlen(name), uk_alloc_get_default());
}

struct uk_vfs_path *uk_posix_vfs_ofile_path(struct uk_ofile *file)
{
	struct uk_vfs_path *path;
	struct uk_fs_path name;

	if (file->destroy == vfs_ofile_destroy && file->context) {
		path = file->context;
		uk_posix_vfs_path_acquire(path);
		return path;
	}
	if (uk_fs_isfs(file->file))
		return ERR2PTR(-ENOENT);
	name = uk_posix_vfs_ofile_name(file);
	if (PTRISERR(name.s))
		return ERR2PTR(PTR2ERR(name.s));
	path = vfs_path_get(NULL, NULL, file->file, name.s, name.len);
	uk_fs_path_release(name);
	return path;
}

static struct uk_vfs_path *vfs_path_root(void)
{
	const struct uk_file *file;
	struct vfs_mount *held, *mount;
	struct uk_vfs_path *path;

	uk_spin_lock(&vfs_ctx->fsroot_lock);
	file = vfs_ctx->fsroot;
	held = vfs_ctx->root_mount;
	uk_file_acquire(file);
	vfs_mount_acquire(held);
	uk_spin_unlock(&vfs_ctx->fsroot_lock);
	mount = held;
	file = vfs_lookupat_mount(file, "", 0, 0, &mount);
	path = PTRISERR(file) ? ERR2PTR(PTR2ERR(file)) :
		vfs_path_get(NULL, mount, file, "", 0);
	if (!PTRISERR(file))
		uk_file_release(file);
	vfs_mount_release(held);
	return path;
}

static struct uk_vfs_path *vfs_path_base(struct uk_ofile *base)
{
	struct uk_vfs_path *path;
	bool current = !base;

	if (current)
		base = vfs_active_cwd();
	if (base->destroy == vfs_ofile_destroy && base->context) {
		path = base->context;
		uk_posix_vfs_path_acquire(path);
		if (current)
			uk_ofile_release(base);
		return path;
	}
	path = vfs_path_root();
	if (PTRISERR(path)) {
		if (current)
			uk_ofile_release(base);
		return path;
	}
	if (base->file != &uk_posix_vfsroot_file &&
	    base->file->state != path->file->state) {
		uk_posix_vfs_path_release(path);
		path = ERR2PTR(-ENOENT);
	}
	if (current)
		uk_ofile_release(base);
	return path;
}

struct vfs_walk {
	struct uk_vfs_path *anchor;
	__u64 resolve;
	unsigned int links;
	int flags;
	mode_t mode;
	bool created;
};

static unsigned int vfs_walk_flags(const struct vfs_walk *walk)
{
	return VFS_LOOKUP_NOFOLLOW | VFS_LOOKUP_IGNMNT_START |
	       ((walk->resolve & UK_VFS_RESOLVE_NO_XDEV) ? VFS_LOOKUP_NO_XDEV
							 : 0) |
	       ((walk->resolve & UK_VFS_RESOLVE_CACHED) ? VFS_LOOKUP_CACHED
							: 0);
}

static struct uk_vfs_path *vfs_path_walk(struct uk_vfs_path *base,
					 const char *name, size_t length,
					 struct vfs_walk *walk, bool terminal,
					 bool nofollow);

static struct uk_vfs_path *vfs_walk_symlink(struct uk_vfs_path *base,
					    const struct uk_file *target,
					    struct vfs_walk *walk,
					    bool terminal)
{
	struct uk_fs_path name;
	struct uk_vfs_path *next = NULL;
	enum uk_fs_link_policy policy = UKFS_LINK_FOLLOW;

	if (walk->resolve & UK_VFS_RESOLVE_NO_SYMLINKS)
		return ERR2PTR(-ELOOP);
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
	if (++walk->links > CONFIG_LIBPOSIX_VFS_MAX_SYMDEPTH)
#else
	if (++walk->links > CONFIG_LIBPOSIX_VFS_MAX_SYMNUM)
#endif
		return ERR2PTR(-ELOOP);
	if (walk->resolve & UK_VFS_RESOLVE_NO_MAGICLINKS)
		policy = UKFS_LINK_NO_MAGIC;
	else if (walk->resolve & UK_VFS_RESOLVE_SCOPED)
		policy = UKFS_LINK_SCOPED;
	if (target->fsops->follow_link)
		next = target->fsops->follow_link(target, policy);
	if (next) {
		if (!PTRISERR(next) &&
		    (walk->resolve & UK_VFS_RESOLVE_NO_XDEV) &&
		    next->mount != base->mount) {
			uk_posix_vfs_path_release(next);
			next = ERR2PTR(-EXDEV);
		}
		goto out;
	}
	name = uk_fs_readlink(target);
	if (PTRISERR(name.s)) {
		next = ERR2PTR(PTR2ERR(name.s));
		goto out;
	}
	uk_posix_vfs_path_acquire(base);
	next = vfs_path_walk(base, name.s, name.len, walk, terminal, false);
	uk_fs_path_release(name);
out:
#if CONFIG_LIBPOSIX_VFS_SYMLIMIT_DEPTH
	--walk->links;
#endif
	return next;
}

static const struct uk_file *vfs_walk_create(const struct uk_file *directory,
					     const char *name, size_t length,
					     struct vfs_walk *walk)
{
	const struct uk_file *file;
	int error;

	if (walk->resolve & UK_VFS_RESOLVE_CACHED)
		return ERR2PTR(-EAGAIN);
	vfs_wlock(directory);
	error = vfs_check_perms(directory, W_OK);
	file = error ? ERR2PTR(error)
		     : uk_fs_createat(directory, name, length, walk->mode,
				      O_EXCL, UKFS_NOTARGET);
	vfs_wunlock(directory);
	if (!PTRISERR(file))
		walk->created = true;
	return file;
}

static struct uk_vfs_path *vfs_path_walk(struct uk_vfs_path *base,
					 const char *name, size_t length,
					 struct vfs_walk *walk, bool terminal,
					 bool nofollow)
{
	const bool directory = length && name[length - 1] == '/';
	struct uk_vfs_path *next;
	struct vfs_mount *mount;
	const struct uk_file *target;
	size_t part, rest;
	int error;

	if (PTRISERR(base))
		return base;
	if (length && name[0] == '/') {
		if (walk->resolve & UK_VFS_RESOLVE_BENEATH) {
			error = -EXDEV;
			goto fail;
		}
		if (walk->resolve & UK_VFS_RESOLVE_IN_ROOT) {
			next = walk->anchor;
			uk_posix_vfs_path_acquire(next);
		} else {
			next = vfs_path_root();
			if (PTRISERR(next)) {
				error = PTR2ERR(next);
				goto fail;
			}
		}
		if ((walk->resolve & UK_VFS_RESOLVE_NO_XDEV) &&
		    next->mount != base->mount) {
			uk_posix_vfs_path_release(next);
			error = -EXDEV;
			goto fail;
		}
		uk_posix_vfs_path_release(base);
		base = next;
	}
	while (length) {
		while (length && *name == '/') {
			name++;
			length--;
		}
		if (!length)
			break;
		if (!uk_fs_isdir(base->file)) {
			error = -ENOTDIR;
			goto fail;
		}
		error = vfs_check_perms(base->file, X_OK);
		if (error)
			goto fail;
		for (part = 0; part < length && name[part] != '/'; part++)
			;
		for (rest = part; rest < length && name[rest] == '/'; rest++)
			;
		if (terminal && rest == length && part < length &&
		    (walk->flags & O_CREAT)) {
			error = -EISDIR;
			goto fail;
		}
		if (part == 1 && name[0] == '.')
			goto advance;
		if (part == 2 && name[0] == '.' && name[1] == '.') {
			if (base == walk->anchor) {
				if (walk->resolve & UK_VFS_RESOLVE_BENEATH) {
					error = -EXDEV;
					goto fail;
				}
				goto advance;
			}
			next = vfs_path_parent(base);
			if ((walk->resolve & UK_VFS_RESOLVE_NO_XDEV) &&
			    next->mount != base->mount) {
				uk_posix_vfs_path_release(next);
				error = -EXDEV;
				goto fail;
			}
			goto replace;
		}
		mount = base->mount;
		target = vfs_lookupat_mount(uk_posix_vfs_path_file(base), name,
					    part, vfs_walk_flags(walk), &mount);
		if (PTRISERR(target) && PTR2ERR(target) == -ENOENT &&
		    terminal && part == length && (walk->flags & O_CREAT)) {
			target = vfs_walk_create(base->file, name, part, walk);
			if (PTRISERR(target) && PTR2ERR(target) == -EEXIST &&
			    !(walk->flags & O_EXCL))
				target =
				vfs_lookupat_mount(uk_posix_vfs_path_file(base),
						   name, part,
						   vfs_walk_flags(walk),
						   &mount);
		}
		if (PTRISERR(target)) {
			error = PTR2ERR(target);
			goto fail;
		}
		if (!walk->created && terminal && rest == length &&
		    (walk->flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
			next = ERR2PTR(-EEXIST);
		} else if (uk_fs_issym(target) &&
			   (!nofollow || part < length)) {
			next = vfs_walk_symlink(base, target, walk,
						terminal && part == length);
		} else {
			next = vfs_path_get(base, mount, target, name, part);
		}
		uk_file_release(target);
replace:
		uk_posix_vfs_path_release(base);
		base = next;
		if (PTRISERR(base))
			return base;
advance:
		name += rest;
		length -= rest;
	}
	if (directory && !uk_fs_isdir(base->file)) {
		error = -ENOTDIR;
		goto fail;
	}
	return base;
fail:
	uk_posix_vfs_path_release(base);
	return ERR2PTR(error);
}

static struct uk_vfs_path *vfs_walk_start(struct uk_ofile *base,
					  const char *name, size_t length,
					  struct vfs_walk *walk, bool nofollow)
{
	struct uk_vfs_path *path;

	if (walk->resolve & UK_VFS_RESOLVE_SCOPED) {
		walk->anchor = vfs_path_base(base);
		if (PTRISERR(walk->anchor))
			return walk->anchor;
		path = walk->anchor;
		uk_posix_vfs_path_acquire(path);
	} else {
		path = length && name[0] == '/' ? vfs_path_root() :
			vfs_path_base(base);
	}
	path = vfs_path_walk(path, name, length, walk, true, nofollow);
	uk_posix_vfs_path_release(walk->anchor);
	walk->anchor = NULL;
	return path;
}

static struct uk_vfs_path *vfs_path_resolve(struct uk_ofile *base,
					    const char *name, size_t length, bool nofollow)
{
	struct vfs_walk walk = {0};

	return vfs_walk_start(base, name, length, &walk, nofollow);
}

static struct uk_ofile *vfs_open_identity(struct uk_vfs_path *identity,
					  unsigned int mode)
{
	const struct uk_file *f = uk_fs_inode(identity->file);
	const struct uk_file *opened = NULL;
	struct uk_ofile *of = NULL;
	struct uk_fs_path name;
	bool filesystem = uk_fs_isfs(f);

	name = uk_posix_vfs_path_name(identity);
	if (PTRISERR(name.s)) {
		of = ERR2PTR(PTR2ERR(name.s));
		goto out;
	}
	if (filesystem)
		mode |= uk_fs_mode_constraints(f);
	else
		mode |= UKFD_O_NOSEEK;
	if (f->ops->open) {
		struct uk_vfs_path *replacement;

		opened = f->ops->open(f, mode);
		if (PTRISERR(opened)) {
			of = ERR2PTR(PTR2ERR(opened));
			opened = NULL;
			goto out_name;
		}
		if (!filesystem) {
			replacement = vfs_path_get(NULL, identity->mount,
						   opened, name.s, name.len);

			uk_posix_vfs_path_release(identity);
			identity = replacement;
			if (PTRISERR(identity)) {
				of = ERR2PTR(PTR2ERR(identity));
				identity = NULL;
				goto out_name;
			}
		}
		f = opened;
	} else if (!filesystem && !(mode & O_PATH)) {
		struct uk_statx status = {0};
		int rc = uk_file_getstat(f, UK_STATX_TYPE, &status);

		if (rc) {
			of = ERR2PTR(rc);
			goto out_name;
		}
		switch (status.stx_mode & S_IFMT) {
		case S_IFBLK:
			mode &= ~UKFD_O_NOSEEK;
			break;
		case S_IFCHR:
			if (f->ops->seek)
				mode &= ~UKFD_O_NOSEEK;
			break;
		default:
			of = ERR2PTR(-ENXIO);
			goto out_name;
		}
	}
	of = uk_ofile_new(f, mode, name.len);
	if (of) {
		memcpy(of->name, name.s, name.len + 1);
		of->context = identity;
		of->destroy = vfs_ofile_destroy;
		identity = NULL;
	}
out_name:
	uk_fs_path_release(name);
	if (opened)
		uk_file_release(opened);
out:
	uk_posix_vfs_path_release(identity);
	return of ? of : ERR2PTR(-ENOMEM);
}

static struct uk_ofile *vfs_open_file(const struct uk_file *file,
				      unsigned int mode,
				      struct uk_ofile *basedir,
				      const char *name, size_t length)
{
	struct uk_vfs_path *path =
		vfs_path_resolve(basedir, name, length,
				 uk_fs_issym(uk_fs_inode(file)));

	if (PTRISERR(path))
		return ERR2PTR(PTR2ERR(path));
	if (path->file->state != uk_fs_inode(file)->state) {
		uk_posix_vfs_path_release(path);
		return ERR2PTR(-ESTALE);
	}
	return vfs_open_identity(path, mode);
}

/*
 * Internal syscalls
 */

/* FS ops */
void uk_sys_sync(void)
{
	uk_mutex_lock(&mtab.lock);
	for (size_t i = 0; i < mtab.top; i++)
		uk_sys_syncfs(mtab.tab[i]->root);
	uk_mutex_unlock(&mtab.lock);
}

int uk_sys_statfs(const char *path, struct statfs *buf)
{
	const struct uk_file *target;
	int r;

	if (unlikely((r = vfs_check_path(path, 0))))
		return r;

	target = vfs_lookup(NULL, path, strlen(path), 0);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);

	r = uk_sys_fstatfs(target, buf);
	uk_file_release(target);
	return r;
}

/* VFS context */
int uk_sys_fchdir(struct uk_ofile *of)
{
	struct uk_ofile *prev;
	int r;

	if (unlikely(!uk_fs_isdir(of->file)))
		return -ENOTDIR;
	if (unlikely((r = vfs_check_perms(of->file, X_OK))))
		return r;

	uk_ofile_acquire(of);
	prev = vfs_set_cwd(of);
	uk_ofile_release(prev);
	return 0;
}

static int vfs_chdir(const char *path)
{
	const struct uk_file *dir;
	struct uk_ofile *odir;
	struct uk_ofile *base = NULL;
	size_t pathlen;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	pathlen = strlen(path);
	dir = vfs_lookupat(vfs_atroot_named(NULL, path, pathlen, &base),
			   path, pathlen, 0);
	if (unlikely(PTRISERR(dir))) {
		if (base)
			uk_ofile_release(base);
		return PTR2ERR(dir);
	}
	/* Create open file description and fchdir */
	odir = vfs_open_file(dir, O_RDONLY | O_PATH, base, path, pathlen);
	uk_file_release(dir);
	if (base)
		uk_ofile_release(base);
	if (unlikely(PTRISERR(odir)))
		return PTR2ERR(odir);
	ret = uk_sys_fchdir(odir);
	uk_ofile_release(odir);
	return ret;
}

int uk_sys_chdir(const char *path)
{
	int result;

	uk_mutex_lock(&vfs_namespace_lock);
	result = vfs_chdir(path);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}

ssize_t uk_sys_getcwd(char *buf, size_t len)
{
	struct uk_ofile *cwd = vfs_active_cwd();
	struct uk_vfs_path *path;
	struct uk_fs_path name;
	ssize_t result;

	uk_mutex_lock(&vfs_namespace_lock);
	path = vfs_path_base(cwd);
	uk_mutex_unlock(&vfs_namespace_lock);
	uk_ofile_release(cwd);
	if (PTRISERR(path))
		return PTR2ERR(path);
	name = vfs_path_cwd_name(path);
	uk_posix_vfs_path_release(path);
	if (PTRISERR(name.s))
		return PTR2ERR(name.s);
	if (name.len >= PATH_MAX) {
		result = -ENAMETOOLONG;
	} else if (name.len + 1 > len) {
		result = -ERANGE;
	} else {
		memcpy(buf, name.s, name.len + 1);
		result = name.len + 1;
	}
	uk_fs_path_release(name);
	return result;
}

int uk_sys_chroot(const char *name)
{
	struct uk_vfs_path *path;
	int error = vfs_check_path(name, 0);

	if (error)
		return error;
	uk_mutex_lock(&vfs_namespace_lock);
	path = vfs_path_resolve(NULL, name, strlen(name), false);
	if (PTRISERR(path)) {
		error = PTR2ERR(path);
		goto out;
	}
	error = uk_fs_isdir(path->file) ? vfs_check_perms(path->file, X_OK) :
		-ENOTDIR;
	if (!error)
		vfs_set_fsroot(path);
	uk_posix_vfs_path_release(path);
out:
	uk_mutex_unlock(&vfs_namespace_lock);
	return error;
}

mode_t uk_sys_umask(mode_t mask)
{
	mask &= 0777;
	return uk_exchange_n(&vfs_ctx->umask, mask);
}

/* Read node metadata */
int uk_sys_faccessat(const struct uk_file *f, const char *path,
		     int mode, int flags)
{
	const int lflags = (flags & AT_SYMLINK_NOFOLLOW) ?
			   VFS_LOOKUP_NOFOLLOW : 0;
	const struct uk_file *target;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), lflags);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);
	ret = (mode == F_OK) ? 0 : vfs_check_access(target, mode);
	uk_file_release(target);
	return ret;
}

int uk_sys_statx(const struct uk_file *f, const char *restrict path, int flags,
		 unsigned int mask, struct uk_statx *restrict statxbuf)
{
	const int lflags = (flags & AT_SYMLINK_NOFOLLOW) ?
			   VFS_LOOKUP_NOFOLLOW : 0;
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely(!statxbuf))
		return -EFAULT;
	if (unlikely((ret = vfs_check_path(path, flags))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), lflags);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);

	/* No perms checking is required on the file itself */
	vfs_tmpofile_init(&of, target, O_RDONLY);
	ret = uk_sys_fstatx(&of, mask, statxbuf);
	uk_file_release(target);
	return ret;
}

int uk_sys_fstatat(const struct uk_file *f, const char *restrict path,
		   struct stat *restrict statbuf, int flags)
{
	struct uk_statx sx;
	int ret;

	if (unlikely(!statbuf))
		return -EFAULT;

	ret = uk_sys_statx(f, path, flags, UK_STATX_BASIC_STATS, &sx);
	if (!ret)
		uk_fdio_statx_cpyout(statbuf, &sx);
	return ret;
}

/* Change node metadata */
int uk_sys_fchownat(const struct uk_file *f, const char *path,
		    uid_t owner, gid_t group, int flags)
{
	const int lflags = (flags & AT_SYMLINK_NOFOLLOW) ?
			   VFS_LOOKUP_NOFOLLOW : 0;
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely(flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)))
		return -EINVAL;
	if (unlikely((ret = vfs_check_path(path, flags))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), lflags);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);

	/* fchown should handle perms checking */
	vfs_tmpofile_init(&of, target, O_WRONLY);
	ret = uk_sys_fchown(&of, owner, group);
	uk_file_release(target);
	return ret;
}

int uk_sys_fchmodat(const struct uk_file *f, const char *path,
		    mode_t mode, int flags)
{
	const int lflags = (flags & AT_SYMLINK_NOFOLLOW) ?
			   VFS_LOOKUP_NOFOLLOW : 0;
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), lflags);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);

	/* fchmod should handle perms checking */
	vfs_tmpofile_init(&of, target, O_WRONLY);
	ret = uk_sys_fchmod(&of, mode);
	uk_file_release(target);
	return ret;
}

int uk_sys_futimesat(const struct uk_file *f, const char *path,
		     const struct timeval *times)
{
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), 0);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);
	if (!times) {
		ret = vfs_check_perms(target, W_OK);
		if (unlikely(ret))
			goto out;
	}

	/* futimes should check perms for times != NULL */
	vfs_tmpofile_init(&of, target, O_WRONLY);
	ret = uk_sys_futimes(&of, times);
out:
	uk_file_release(target);
	return ret;
}

int uk_sys_utime(const char *path, const struct utimbuf *times)
{
	if (times) {
		struct timeval t[2] = {
			{ .tv_sec = times->actime },
			{ .tv_sec = times->modtime }
		};

		return uk_sys_futimesat(NULL, path, t);
	}
	return uk_sys_futimesat(NULL, path, NULL);
}

int uk_sys_utimensat(const struct uk_file *f, const char *path,
		     const struct timespec *times, int flags)
{
	const int lflags = (flags & AT_SYMLINK_NOFOLLOW) ?
			   VFS_LOOKUP_NOFOLLOW : 0;
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	target = vfs_lookup(f, path, strlen(path), lflags);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);
	if (!times ||
	    ((times[0].tv_nsec == UTIME_NOW ||
	      times[0].tv_nsec == UTIME_OMIT) &&
	     (times[1].tv_nsec == UTIME_NOW ||
	      times[1].tv_nsec == UTIME_OMIT))) {
		ret = vfs_check_perms(target, W_OK);
		if (unlikely(ret))
			goto out;
	}

	/* futimens should check perms for custom times */
	vfs_tmpofile_init(&of, target, O_WRONLY);
	ret = uk_sys_futimens(&of, times);
out:
	uk_file_release(target);
	return ret;
}

/* Read node contents */
ssize_t uk_sys_getdents64(const struct uk_file *f, size_t *curp,
			  void *dirp, size_t count, int flags)
{
	ssize_t ret;

	UK_ASSERT(curp);
	if (unlikely(!dirp))
		return -EFAULT;
	if (unlikely(!uk_fs_isdir(f)))
		return -ENOTDIR;

	vfs_rlock(f);
	ret = uk_fs_listdir(f, curp, dirp, count, flags);
	vfs_runlock(f);
	return ret;
}

ssize_t uk_sys_readlinkat(const struct uk_file *f, const char *restrict path,
			  char *restrict buf, size_t bufsz)
{
	const struct uk_file *target;
	struct uk_fs_path linkpath;
	ssize_t ret;

	if (unlikely(!bufsz))
		return -EINVAL;
	if (unlikely(!buf))
		return -EFAULT;
	ret = vfs_check_path(path, AT_EMPTY_PATH);
	if (unlikely(ret))
		return ret;

	if (!*path) {
		if (!f || !uk_fs_issym(f))
			return -ENOENT;
		uk_file_acquire(f);
		target = f;
	} else {
		target = vfs_lookup(f, path, strlen(path), VFS_LOOKUP_NOFOLLOW);
	}
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);
	if (unlikely(!uk_fs_issym(target))) {
		ret = -EINVAL;
		goto out;
	}

	/* Symlinks uniquely don't have meaningful permissions to check */
	linkpath = uk_fs_readlink(target);
	if (unlikely(PTRISERR(linkpath.s))) {
		ret = PTR2ERR(linkpath.s);
		goto out;
	}
	if (linkpath.len >= PATH_MAX) {
		ret = -ENAMETOOLONG;
	} else {
		ret = MIN(bufsz, linkpath.len);
		if (uk_nofault_memcpy(buf, linkpath.s, ret, 0) != (size_t)ret)
			ret = -EFAULT;
	}
	uk_fs_path_release(linkpath);
out:
	uk_file_release(target);
	return ret;
}

/* Change node contents */
int uk_sys_truncate(const char *path, off_t len)
{
	const struct uk_file *target;
	struct uk_ofile of;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	target = vfs_lookup(NULL, path, strlen(path), 0);
	if (unlikely(PTRISERR(target)))
		return PTR2ERR(target);

	ret = vfs_check_perms(target, W_OK);
	if (unlikely(ret))
		goto out;

	vfs_tmpofile_init(&of, target, O_WRONLY);
	ret = uk_sys_ftruncate(&of, len);
out:
	uk_file_release(target);
	return ret;
}

/* Create node w/o opening */
int uk_sys_mkdirat(const struct uk_file *f, const char *path, mode_t mode)
{
	const struct uk_file *newdir;
	const struct uk_file *dir;
	struct uk_fs_poslen p;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	/* Lookup parent of path */
	/* ignore trailing slashes (uniquely among *dir syscalls) */
	p = uk_fs_path_len_trail(path, 1);
	newdir = vfs_lookup(f, path, p.len, VFS_LOOKUP_NOFOLLOW);
	if (!PTRISERR(newdir)) {
		uk_file_release(newdir);
		return -EEXIST;
	}
	ret = PTR2ERR(newdir);
	if (ret != -ENOENT)
		return ret;

	dir = vfs_lookup(f, path, p.pos, 0);
	if (unlikely(PTRISERR(dir)))
		return PTR2ERR(dir);

	if (unlikely(!uk_fs_isdir(dir))) {
		ret = -ENOTDIR;
		goto out;
	}

	mode &= ~S_IFMT;
	mode |= S_IFDIR;

	vfs_wlock(dir);
	ret = vfs_check_perms(dir, W_OK);
	if (!ret)
		newdir = uk_fs_createat(dir, &path[p.pos], p.len - p.pos,
					vfs_apply_umask(mode), O_EXCL,
					UKFS_NOTARGET);
	vfs_wunlock(dir);
	if (unlikely(ret))
		goto out;
	if (unlikely(PTRISERR(newdir))) {
		ret = PTR2ERR(newdir);
		goto out;
	}

	uk_file_release(newdir);
out:
	uk_file_release(dir);
	return ret;
}

int uk_sys_linkat(const struct uk_file *olddir, const char *oldpath,
		  const struct uk_file *newdir, const char *newpath, int flags)
{
	union uk_fs_create_target target;
	const struct uk_file *dest;
	const struct uk_file *newlink;
	struct uk_fs_poslen np;
	int ret;

	if (unlikely((ret = vfs_check_path(newpath, 0))))
		return ret;
	if (unlikely(flags & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)))
		return -EINVAL;
	if (unlikely((ret = vfs_check_path(oldpath, flags))))
		return ret;

	target.file = vfs_lookup(olddir, oldpath, strlen(oldpath),
				 (flags & AT_SYMLINK_FOLLOW) ?
				 0 : VFS_LOOKUP_NOFOLLOW);
	if (unlikely(PTRISERR(target.file)))
		return PTR2ERR(target.file);

	np = uk_fs_path_len_trail(newpath, 0);
	dest = vfs_lookup(newdir, newpath, np.pos, 0);
	if (unlikely(PTRISERR(dest))) {
		ret = PTR2ERR(dest);
		goto out_targ;
	}

	if (unlikely(!uk_fs_isdir(dest))) {
		ret = -ENOTDIR;
		goto out_dest;
	}

	vfs_wlock(dest);
	ret = vfs_check_perms(dest, W_OK);
	if (!ret)
		newlink = uk_fs_createat(dest,
					 &newpath[np.pos], np.len - np.pos,
					 S_IFMT, O_EXCL, target);
	vfs_wunlock(dest);
	if (unlikely(ret))
		goto out_dest;
	if (unlikely(PTRISERR(newlink))) {
		ret = PTR2ERR(newlink);
		goto out_dest;
	}

	uk_file_release(newlink);
out_dest:
	uk_file_release(dest);
out_targ:
	uk_file_release(target.file);
	return ret;
}

int uk_sys_symlinkat(const char *targetpath, const struct uk_file *f,
		     const char *linkpath)
{
	const union uk_fs_create_target target = { .path = targetpath };
	const struct uk_file *dir;
	const struct uk_file *newsym;
	struct uk_fs_poslen p;
	int ret;

	if (unlikely((ret = vfs_check_path(targetpath, 0))))
		return ret;
	if (unlikely((ret = vfs_check_path(linkpath, 0))))
		return ret;

	p = uk_fs_path_len_trail(linkpath, 0);
	dir = vfs_lookup(f, linkpath, p.pos, 0);
	if (unlikely(PTRISERR(dir)))
		return PTR2ERR(dir);

	if (unlikely(!uk_fs_isdir(dir))) {
		ret = -ENOTDIR;
		goto out;
	}

	vfs_wlock(dir);
	ret = vfs_check_perms(dir, W_OK);
	if (!ret)
		newsym = uk_fs_createat(dir, &linkpath[p.pos], p.len - p.pos,
					S_IFLNK | 0777, O_EXCL, target);
	vfs_wunlock(dir);
	if (unlikely(ret))
		goto out;
	if (unlikely(PTRISERR(newsym))) {
		ret = PTR2ERR(newsym);
		goto out;
	}

	uk_file_release(newsym);
out:
	uk_file_release(dir);
	return ret;
}

int uk_sys_mknodat(const struct uk_file *f, const char *path,
		   mode_t mode, dev_t dev __unused)
{
	const mode_t ftype = mode & S_IFMT;
	const struct uk_file *dir;
	const struct uk_file *newf;
	struct uk_fs_poslen p;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;

	/* By default create regular file */
	if (!ftype)
		mode |= S_IFREG;
	else
		switch (ftype) {
		case S_IFREG:
		case S_IFSOCK:
		case S_IFIFO:
			break;
		case S_IFCHR:
		case S_IFBLK:
			uk_pr_warn("Special files not supported\n");
			__fallthrough;
		default:
			return -EINVAL;
		}

	p = uk_fs_path_len_trail(path, 0);
	dir = vfs_lookup(f, path, p.pos, 0);
	if (unlikely(PTRISERR(dir)))
		return PTR2ERR(dir);

	if (unlikely(!uk_fs_isdir(dir))) {
		ret = -ENOTDIR;
		goto out;
	}

	vfs_wlock(dir);
	ret = vfs_check_perms(dir, W_OK);
	if (!ret)
		newf = uk_fs_createat(dir, &path[p.pos], p.len - p.pos,
				      vfs_apply_umask(mode), O_EXCL,
				      UKFS_NOTARGET);
	vfs_wunlock(dir);
	if (unlikely(ret))
		goto out;
	if (unlikely(PTRISERR(newf))) {
		ret = PTR2ERR(newf);
		goto out;
	}

	uk_file_release(newf);
out:
	uk_file_release(dir);
	return ret;
}

/* Remove/rename node */
static int vfs_unlinkat(const struct uk_file *f, const char *path, int flags)
{
	const struct uk_file *dir;
	struct uk_fs_poslen p;
	int uflags;
	int ret;

	if (unlikely((ret = vfs_check_path(path, 0))))
		return ret;
	if (unlikely(flags & ~AT_REMOVEDIR))
		return -EINVAL;

	if (flags & AT_REMOVEDIR) {
		uflags = UKFS_UNLINK_DIR;
	} else {
		uflags = 0;
#if !CONFIG_LIBPOSIX_VFS_RELAX_UNLINK
		uflags |= UKFS_UNLINK_NODIR;
#endif /* !CONFIG_LIBPOSIX_VFS_RELAX_UNLINK */
	}
#if !CONFIG_LIBPOSIX_VFS_RELAX_RMDIR
	uflags |= UKFS_UNLINK_EMPTY;
#endif /* !CONFIG_LIBPOSIX_VFS_RELAX_RMDIR */

	/* Get parent of path; rmdir ignores trailing slashes in path */
	p = uk_fs_path_len_trail(path, !!(flags & AT_REMOVEDIR));
	dir = vfs_lookup(f, path, p.pos, 0);
	if (unlikely(PTRISERR(dir)))
		return PTR2ERR(dir);

	if (unlikely(!uk_fs_isdir(dir))) {
		ret = -ENOTDIR;
		goto out;
	}

	vfs_wlock(dir);
	ret = vfs_check_perms(dir, W_OK);
	if (!ret)
		ret = uk_fs_unlinkat(dir, &path[p.pos], p.len - p.pos, uflags);
	if (!ret)
		vfs_path_unlinked(dir, &path[p.pos], p.len - p.pos);
	vfs_wunlock(dir);
out:
	uk_file_release(dir);
	return ret;
}

/* Need to take care to prevent deadlocks when locking both src and dest dirs */
static
void vfs_rename_lock(const struct uk_file *sdir, const struct uk_file *ddir)
{
	const struct uk_file *d1, *d2;

	/* We always lock & unlock in ascending state pointer order */
	if (sdir->state <= ddir->state) {
		d1 = sdir;
		d2 = ddir;
	} else {
		d1 = ddir;
		d2 = sdir;
	}
	vfs_wlock(d1);
	/* We do not lock the 2nd directory if it shares state with the 1st */
	if (d1->state != d2->state)
		vfs_wlock(d2);
}

static
void vfs_rename_unlock(const struct uk_file *sdir, const struct uk_file *ddir)
{
	const struct uk_file *d1, *d2;

	/* We always lock & unlock in ascending state pointer order */
	if (sdir->state <= ddir->state) {
		d1 = sdir;
		d2 = ddir;
	} else {
		d1 = ddir;
		d2 = sdir;
	}
	/* We do not lock the 2nd directory if it shares state with the 1st */
	if (d1->state != d2->state)
		vfs_wunlock(d2);
	vfs_wunlock(d1);
}

static int vfs_renameat(const struct uk_file *olddir, const char *oldpath,
			const struct uk_file *newdir, const char *newpath,
			int flags)
{
	const struct uk_file *sdir;
	const struct uk_file *ddir;
	struct uk_fs_poslen op;
	struct uk_fs_poslen np;
	int ret;

	if (unlikely((ret = vfs_check_path(oldpath, 0))))
		return ret;
	if (unlikely((ret = vfs_check_path(newpath, 0))))
		return ret;

	op = uk_fs_path_len_trail(oldpath, 1);
	sdir = vfs_lookup(olddir, oldpath, op.pos, 0);
	if (unlikely(PTRISERR(sdir)))
		return PTR2ERR(sdir);
	if (unlikely(!uk_fs_isdir(sdir))) {
		ret = -ENOTDIR;
		goto out_src;
	}

	np = uk_fs_path_len_trail(newpath, 0);
	ddir = vfs_lookup(newdir, newpath, np.pos, 0);
	if (unlikely(PTRISERR(ddir))) {
		ret = PTR2ERR(ddir);
		goto out_src;
	}
	if (unlikely(!uk_fs_isdir(ddir))) {
		ret = -ENOTDIR;
		goto out_dest;
	}

	vfs_rename_lock(sdir, ddir);
	ret = vfs_check_perms(sdir, W_OK);
	if (unlikely(ret))
		goto out_unlock;

	ret = vfs_check_perms(ddir, W_OK);
	if (unlikely(ret))
		goto out_unlock;

	ret = uk_fs_renameat(sdir, &oldpath[op.pos], op.len - op.pos,
			     ddir, &newpath[np.pos], np.len - np.pos,
			     flags);
out_unlock:
	vfs_rename_unlock(sdir, ddir);
out_dest:
	uk_file_release(ddir);
out_src:
	uk_file_release(sdir);
	return ret;
}

/* Mounting */

static struct vfs_mount *vfs_mount_new(struct uk_vfs_path *point,
				       const struct uk_file *root)
{
	struct vfs_mount *mount =
	    uk_zalloc(uk_alloc_get_default(), sizeof(*mount));

	if (!mount)
		return NULL;
	mount->root = root;
	mount->point = point->file;
	mount->parent = point->mount;
	uk_refcount_init(&mount->references, 1);
	uk_file_acquire(root);
	uk_file_acquire(mount->point);
	vfs_mount_acquire(mount->parent);
	return mount;
}

static int vfs_mount(const char *name, const struct uk_file *newroot)
{
	struct uk_vfs_path *path;
	struct vfs_mount *mount;
	int error = vfs_check_path(name, 0);

	if (error)
		return error;
	path = vfs_path_resolve(NULL, name, strlen(name), false);
	if (PTRISERR(path))
		return PTR2ERR(path);
	mount = vfs_mount_new(path, newroot);
	if (!mount) {
		error = -ENOMEM;
		goto out_path;
	}
	uk_mutex_lock(&mtab.lock);
	if (mtab.top == CONFIG_LIBPOSIX_VFS_MAX_MOUNTS) {
		error = -EMFILE;
		goto out_unlock;
	}
	error = uk_fs_mountat(path->file, &newroot);
	if (error)
		goto out_unlock;
	if (uk_fs_isdir(newroot)) {
		error = uk_fs_graft(newroot, path->file);
		if (error) {
			const struct uk_file *prev = NULL;
			int rollback __maybe_unused =
			    uk_fs_mountat(path->file, &prev);

			UK_ASSERT(!rollback && prev == newroot);
			uk_file_release(prev);
			goto out_unlock;
		}
	}
	mount->attached = true;
	mtab.tab[mtab.top++] = mount;
	mount = NULL;
out_unlock:
	uk_mutex_unlock(&mtab.lock);
	vfs_mount_release(mount);
out_path:
	uk_posix_vfs_path_release(path);
	return error;
}

int uk_posix_vfs_mount(const char *path, const struct uk_file *newroot)
{
	int result;

	uk_mutex_lock(&vfs_namespace_lock);
	result = vfs_mount(path, newroot);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}

static
const struct uk_file *vfs_vopen(const char *source, const char *fstype,
				unsigned long flags, void *data)
{
	const struct uk_fs_drv *drv = uk_fs_driver(fstype);
	union uk_fs_vopen_vol vol = UK_FS_VOPEN_NULLVOL;
	union uk_fs_vopen_data vdata = UK_FS_VOPEN_NULLDATA;
	size_t fmt = UK_FS_VOPEN_VOL_IGNORE | UK_FS_VOPEN_DATA_IGNORE;
	const struct uk_file *f;
	int err = 0;

	if (unlikely(!drv))
		return ERR2PTR(-ENODEV);

	/* Process source, if required */
	if ((drv->formats & UK_FS_VOPEN_VOL_MASK) == UK_FS_VOPEN_VOL_IGNORE)
		goto vol_done; /* Don't bother */
	if ((drv->formats & UK_FS_VOPEN_VOL_FILE) &&
	    source && source[0] == '/') {
		/* source is absolute path; try to open */
		f = vfs_lookup(NULL, source, strlen(source), 0);
		if (!PTRISERR(f)) {
			vol.file = f;
			fmt |= UK_FS_VOPEN_VOL_FILE;
			goto vol_done;
		}
		/* On failure remember error & try other options */
		err = PTR2ERR(f);
	}
	if (drv->formats & UK_FS_VOPEN_VOL_RAW) {
		vol.raw = source;
		fmt |= UK_FS_VOPEN_VOL_RAW;
		goto vol_done;
	}
	if (err)
		return ERR2PTR(err);
	else
		return ERR2PTR(-EINVAL); /* Unsupported source expected */
vol_done:
	err = 0;

	/* Process data, if required */
	if ((drv->formats & UK_FS_VOPEN_DATA_MASK) == UK_FS_VOPEN_DATA_IGNORE)
		goto data_done; /* Don't bother */
	if ((drv->formats & UK_FS_VOPEN_DATA_FILE) &&
	    data && ((const char *)data)[0] == '/') {
		/* data is absolute path; try to open */
		f = vfs_lookup(NULL, data, strlen(data), 0);
		if (!PTRISERR(f)) {
			vdata.file = f;
			fmt |= UK_FS_VOPEN_DATA_FILE;
			goto data_done;
		}
		/* On failure try to pass verbatim */
		err = PTR2ERR(f);
	}
	if (drv->formats & UK_FS_VOPEN_DATA_RAW) {
		vdata.raw = data;
		fmt |= UK_FS_VOPEN_DATA_RAW;
		goto data_done;
	}
	if (err) {
		f = ERR2PTR(err);
		goto out_vol;
	}
data_done:

	/* Call vopen, drop refs & output */
	f = drv->vopen(vol, flags, vdata, fmt);

	if (fmt & UK_FS_VOPEN_DATA_FILE)
		uk_file_release(vdata.file);
out_vol:
	if (fmt & UK_FS_VOPEN_VOL_FILE)
		uk_file_release(vol.file);
	return f;
}

static int vfs_mount_request(const char *source, const char *targetpath,
			     const char *fstype, unsigned long flags,
			     void *data)
{
	const struct uk_file *newroot;
	int ret;

	if (unlikely(flags & MS_REMOUNT)) {
		uk_pr_err("MS_REMOUNT not supported\n");
		return -ENOSYS;
	}
	if (unlikely(flags & MS_MOVE)) {
		uk_pr_err("MS_MOVE not supported\n");
		return -ENOSYS;
	}
	/* TODO: require privileges for (un)mounting */

	if (flags & MS_BIND) {
		const struct uk_file *src;

		if (unlikely((ret = vfs_check_path(source, 0))))
			return ret;

		src = vfs_lookup(NULL, source, strlen(source), 0);

		if (unlikely(PTRISERR(src)))
			return PTR2ERR(src);
		newroot = uk_fs_rebind(src, flags, data);
		uk_file_release(src);
	} else {
		newroot = vfs_vopen(source, fstype, flags, data);
	}
	if (unlikely(PTRISERR(newroot)))
		return PTR2ERR(newroot);

	ret = vfs_mount(targetpath, newroot);
	uk_file_release(newroot);
	return ret;
}

int uk_sys_mount(const char *source, const char *targetpath, const char *fstype,
		 unsigned long flags, void *data)
{
	int result;

	uk_mutex_lock(&vfs_namespace_lock);
	result = vfs_mount_request(source, targetpath, fstype, flags, data);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}

static int vfs_umount(const char *name, int flags __unused)
{
	struct uk_vfs_path *path;
	struct vfs_mount *removed = NULL;
	size_t index;
	int error = vfs_check_path(name, 0);

	if (error)
		return error;
	path = vfs_path_resolve(NULL, name, strlen(name), false);
	if (PTRISERR(path))
		return PTR2ERR(path);
	uk_mutex_lock(&mtab.lock);
	for (index = 0; index < mtab.top; index++)
		if (mtab.tab[index] == path->mount &&
		    mtab.tab[index]->root->state == path->file->state)
			break;
	if (index == mtab.top) {
		error = -EINVAL;
		goto out;
	}
	for (size_t i = 0; i < mtab.top; i++)
		if (mtab.tab[i]->parent == path->mount) {
			error = -EBUSY;
			goto out;
		}
	error = uk_fs_sync(mtab.tab[index]->root);
	if (!error) {
		const struct uk_file *prev = NULL;

		error = uk_fs_mountat(mtab.tab[index]->point, &prev);
		if (error)
			goto out;
		UK_ASSERT(prev == mtab.tab[index]->root);
		uk_file_release(prev);
		removed = mtab.tab[index];
		removed->attached = false;
		for (size_t i = index + 1; i < mtab.top; i++)
			mtab.tab[i - 1] = mtab.tab[i];
		mtab.top--;
	}
out:
	uk_mutex_unlock(&mtab.lock);
	vfs_mount_release(removed);
	uk_posix_vfs_path_release(path);
	return error;
}

int uk_sys_umount(const char *targetpath, int flags)
{
	int result;

	uk_mutex_lock(&vfs_namespace_lock);
	result = vfs_umount(targetpath, flags);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}

/* Open */

static
const struct uk_file *vfs_createx(struct uk_ofile *of,
				  const char *path, mode_t mode,
				  size_t *pathlen,
				  struct uk_ofile **base)
{
	const struct uk_file *dir;
	const struct uk_file *newf;
	struct uk_fs_poslen p;
	int r;

	p = uk_fs_path_len_trail(path, 0);
	*pathlen = p.len;
	dir = vfs_lookupat(vfs_atroot_named(of, path, p.pos, base),
			   path, p.pos, 0);
	if (unlikely(PTRISERR(dir)))
		return dir;
	if (unlikely(!uk_fs_isdir(dir))) {
		newf = ERR2PTR(-ENOTDIR);
		goto out;
	}

	vfs_wlock(dir);
	r = vfs_check_perms(dir, W_OK);
	if (unlikely(r))
		newf = ERR2PTR(r);
	else
		newf = uk_fs_createat(dir, &path[p.pos], p.len - p.pos,
				      mode, O_EXCL, UKFS_NOTARGET);
	vfs_wunlock(dir);
out:
	uk_file_release(dir);
	return newf;
}

const struct uk_file *uk_posix_vfs_socket_create(const char *path)
{
	struct uk_ofile *base = NULL;
	const struct uk_file *file;
	size_t length;
	int error = vfs_check_path(path, 0);

	if (error)
		return ERR2PTR(error);
	uk_mutex_lock(&vfs_namespace_lock);
	file = vfs_createx(NULL, path, vfs_apply_umask(S_IFSOCK | 0777),
			   &length, &base);
	if (base)
		uk_ofile_release(base);
	uk_mutex_unlock(&vfs_namespace_lock);
	return file;
}

static struct uk_ofile *vfs_open_tmpfile(struct uk_vfs_path *directory,
					 int flags, mode_t mode)
{
	struct uk_vfs_path *identity;
	const struct uk_file *file;
	struct uk_statx status;
	char name[1 + 20 + 1];
	int error, length;

	vfs_wlock(directory->file);
	error = vfs_check_perms(directory->file, W_OK | X_OK);
	file = error ? ERR2PTR(error) :
		uk_fs_createat(directory->file, "", 0, mode,
			       flags & (O_TMPFILE | O_EXCL), UKFS_NOTARGET);
	vfs_wunlock(directory->file);
	if (PTRISERR(file))
		return ERR2PTR(PTR2ERR(file));
	error = uk_file_getstat(file, UK_STATX_INO, &status);
	if (error) {
		uk_file_release(file);
		return ERR2PTR(error);
	}
	length = snprintf(name, sizeof(name), "#%llu",
			  (unsigned long long)status.stx_ino);
	identity = vfs_path_get(directory, directory->mount, file,
				name, length);
	uk_file_release(file);
	if (PTRISERR(identity))
		return ERR2PTR(PTR2ERR(identity));
	vfs_path_mark_deleted(identity);
	return vfs_open_identity(identity, flags & UKFD_MODE_MASK);
}

static struct uk_ofile *vfs_openat(struct uk_ofile *of, const char *path,
				   int flags, mode_t mode, __u64 resolve)
{
	struct vfs_walk walk = {.resolve = resolve};
	struct uk_vfs_path *identity;
	const struct uk_file *target;
	struct uk_ofile *ret;
	int err;

	if ((err = vfs_check_path(path, 0)))
		return ERR2PTR(err);
	if (flags & O_PATH)
		flags &= O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC;
	else
		flags |= O_LARGEFILE;
	if ((flags & O_CREAT) && (flags & O_DIRECTORY))
		return ERR2PTR(-EINVAL);
	walk.flags = flags;
	walk.mode = vfs_apply_umask((mode & ~S_IFMT) | S_IFREG);
	identity = vfs_walk_start(of, path, strlen(path), &walk,
				  !!(flags & O_NOFOLLOW));
	if (PTRISERR(identity))
		return ERR2PTR(PTR2ERR(identity));
	target = identity->file;
	if ((flags & O_CREAT) && uk_fs_isdir(target)) {
		ret = ERR2PTR((flags & O_EXCL) && path[strlen(path) - 1] != '/'
				  ? -EEXIST
				  : -EISDIR);
		goto out_identity;
	}
	if ((flags & O_DIRECTORY) && !uk_fs_isdir(target)) {
		ret = ERR2PTR(-ENOTDIR);
		goto out_identity;
	}
	if ((flags & O_TMPFILE) == O_TMPFILE) {
		ret = vfs_open_tmpfile(identity, flags, walk.mode);
		goto out_identity;
	}
	if (!walk.created && !(flags & O_PATH)) {
		if (uk_fs_isdir(target)) {
			if (unlikely((flags & O_WRONLY) || (flags & O_RDWR))) {
				ret = ERR2PTR(-EISDIR);
				goto out_identity;
			}
			if (unlikely((err = vfs_check_perms(target, R_OK)))) {
				ret = ERR2PTR(err);
				goto out_identity;
			}
		}
		if (unlikely(uk_fs_issym(target))) {
			ret = ERR2PTR(-ELOOP);
			goto out_identity;
		}
		if (!uk_fs_isdir(target) && !uk_fs_issym(target)) {
			mode_t desired = (flags & O_WRONLY) ? W_OK : R_OK;

			if (flags & O_RDWR)
				desired |= R_OK | W_OK;
			err = vfs_check_perms(target, desired);
			if (unlikely(err)) {
				ret = ERR2PTR(err);
				goto out_identity;
			}
		}
	}
	if (!(flags & O_PATH) && uk_fs_isfs(target)) {
		struct uk_statx status;

		err = uk_file_getstat(target, UK_STATX_TYPE, &status);
		if (err) {
			ret = ERR2PTR(err);
			goto out_identity;
		}
		if ((status.stx_mode & S_IFMT) == S_IFSOCK) {
			ret = ERR2PTR(-ENXIO);
			goto out_identity;
		}
	}
	ret = vfs_open_identity(identity, flags & UKFD_MODE_MASK);
	identity = NULL;
out_identity:
	uk_posix_vfs_path_release(identity);

	if (!PTRISERR(ret) && uk_fs_isfs(ret->file) && (flags & O_TRUNC) &&
	    ((flags & O_WRONLY) || (flags & O_RDWR))) {
		struct uk_statx status;

		vfs_rlock(ret->file);
		err = uk_file_getstat(ret->file, UK_STATX_TYPE, &status);
		vfs_runlock(ret->file);
		if (!err && (status.stx_mode & S_IFMT) != S_IFIFO)
			err = uk_sys_ftruncate(ret, 0);
		if (unlikely(err)) {
			UK_ASSERT(err < 0);
			uk_ofile_release(ret);
			ret = ERR2PTR(err);
		}
	}

	return ret;
}

static struct uk_ofile *vfs_open_finish(struct uk_ofile *opened)
{
	if (!PTRISERR(opened) && !(opened->mode & O_PATH)) {
		const struct uk_file *endpoint;

		endpoint = vfs_fifo_open(opened->file, opened->mode);
		if (PTRISERR(endpoint)) {
			uk_ofile_release(opened);
			return ERR2PTR(PTR2ERR(endpoint));
		}
		if (endpoint) {
			uk_file_release(opened->file);
			opened->file = endpoint;
			opened->mode |= UKFD_O_NOSEEK | UKFD_O_NOIOLOCK;
		}
	}
	return opened;
}

static const __u64 vfs_valid_open_flags =
	O_ACCMODE | O_APPEND | O_ASYNC | O_CLOEXEC | O_CREAT | O_DIRECT |
	O_DIRECTORY | O_DSYNC | O_EXCL | O_LARGEFILE | O_NOATIME | O_NOCTTY |
	O_NOFOLLOW | O_NONBLOCK | O_PATH | O_SYNC | O_TMPFILE | O_TRUNC;

int uk_posix_vfs_open_validate(const struct uk_vfs_open_how *how)
{
	const __u64 valid_resolve = UK_VFS_RESOLVE_NO_XDEV |
		UK_VFS_RESOLVE_NO_MAGICLINKS | UK_VFS_RESOLVE_NO_SYMLINKS |
		UK_VFS_RESOLVE_SCOPED | UK_VFS_RESOLVE_CACHED;
	const __u64 tmpfile = O_TMPFILE & ~O_DIRECTORY;
	const bool temporary = !!(how->flags & tmpfile);

	if ((how->flags & ~vfs_valid_open_flags) ||
	    (how->resolve & ~valid_resolve))
		return -EINVAL;
	if ((how->resolve & UK_VFS_RESOLVE_SCOPED) == UK_VFS_RESOLVE_SCOPED)
		return -EINVAL;
	if ((how->flags & O_PATH) &&
	    (how->flags & ~(O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC)))
		return -EINVAL;
	if (how->mode & ~07777ULL)
		return -EINVAL;
	if (how->mode && !(how->flags & O_CREAT) && !temporary)
		return -EINVAL;
	if (temporary && ((how->flags & O_TMPFILE) != O_TMPFILE ||
			  !(how->flags & (O_WRONLY | O_RDWR))))
		return -EINVAL;
	if ((how->flags & O_CREAT) && (how->flags & O_DIRECTORY))
		return -EINVAL;
	if ((how->resolve & UK_VFS_RESOLVE_CACHED) &&
	    ((how->flags & (O_CREAT | O_TRUNC)) || temporary))
		return -EAGAIN;
	return 0;
}

int uk_posix_vfs_open_how(int flags, mode_t mode, struct uk_vfs_open_how *how)
{
	*how = (struct uk_vfs_open_how){
		.flags = (unsigned int)flags & vfs_valid_open_flags
	};
	if (how->flags & O_PATH)
		how->flags &= O_PATH | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC;
	else if (how->flags & (O_CREAT | (O_TMPFILE & ~O_DIRECTORY)))
		how->mode = mode & 07777;
	return uk_posix_vfs_open_validate(how);
}

struct uk_ofile *uk_sys_openat(struct uk_ofile *of, const char *path,
			       int flags, mode_t mode)
{
	struct uk_ofile *opened;

	uk_mutex_lock(&vfs_namespace_lock);
	opened = vfs_openat(of, path, flags, mode, 0);
	uk_mutex_unlock(&vfs_namespace_lock);
	return vfs_open_finish(opened);
}

struct uk_ofile *uk_sys_openat2(struct uk_ofile *of, const char *path,
				const struct uk_vfs_open_how *how)
{
	struct uk_ofile *opened;
	int error = uk_posix_vfs_open_validate(how);

	if (error)
		return ERR2PTR(error);
	if (how->resolve & UK_VFS_RESOLVE_CACHED) {
		if (!uk_mutex_trylock(&vfs_namespace_lock))
			return ERR2PTR(-EAGAIN);
	} else {
		uk_mutex_lock(&vfs_namespace_lock);
	}
	opened = vfs_openat(of, path, (int)how->flags, (mode_t)how->mode,
			    how->resolve);
	uk_mutex_unlock(&vfs_namespace_lock);
	return vfs_open_finish(opened);
}

struct uk_ofile *uk_posix_vfs_exec_open(const char *name,
					struct uk_vfs_path **identity)
{
	struct uk_vfs_path *path;
	struct uk_ofile *opened;
	struct uk_statx metadata;
	size_t length;
	bool lock;
	int rc;

	*identity = NULL;
	rc = vfs_check_path(name, 0);
	if (rc)
		return ERR2PTR(rc);
	length = strlen(name);
	uk_mutex_lock(&vfs_namespace_lock);
	path = vfs_path_resolve(NULL, name, length, false);
	if (PTRISERR(path)) {
		opened = ERR2PTR(PTR2ERR(path));
		goto out;
	}
	lock = !uk_fs_isfs(path->file) || _vfs_should_lock(path->file);
	if (lock)
		uk_file_rlock(path->file);
	rc = uk_file_getstat(path->file, UK_STATX_TYPE | UK_STATX_MODE,
			     &metadata);
	if (lock)
		uk_file_runlock(path->file);
	if (!rc && (!S_ISREG(metadata.stx_mode) ||
		    !(metadata.stx_mode & 0111)))
		rc = -EACCES;
	if (!rc)
		rc = vfs_check_access(path->file, X_OK);
	if (rc) {
		opened = ERR2PTR(rc);
		goto out_path;
	}
	opened = vfs_open_file(path->file, O_RDONLY, NULL, name, length);
	if (PTRISERR(opened))
		goto out_path;
	*identity = path;
	goto out;
out_path:
	uk_posix_vfs_path_release(path);
out:
	uk_mutex_unlock(&vfs_namespace_lock);
	return opened;
}

int uk_sys_unlinkat(const struct uk_file *file, const char *path, int flags)
{
	int result;

	uk_mutex_lock(&vfs_namespace_lock);
	result = vfs_unlinkat(file, path, flags);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}

int uk_sys_renameat(struct uk_ofile *oldof, const char *oldpath,
		    struct uk_ofile *newof, const char *newpath, int flags)
{
	struct uk_vfs_path *source = NULL, *destination = NULL;
	struct vfs_path_move *move = NULL;
	struct uk_fs_poslen from, to;
	const struct uk_file *first, *second;
	int result;

	if (!oldpath || !newpath)
		return -EFAULT;
	uk_mutex_lock(&vfs_namespace_lock);
	from = uk_fs_path_len_trail(oldpath, 1);
	to = uk_fs_path_len_trail(newpath, 0);
	source = vfs_path_resolve(oldof,
				  oldpath, from.pos, false);
	if (PTRISERR(source)) {
		result = PTR2ERR(source);
		source = NULL;
		goto out;
	}
	destination = vfs_path_resolve(newof,
				       newpath, to.pos, false);
	if (PTRISERR(destination)) {
		result = PTR2ERR(destination);
		destination = NULL;
		goto out;
	}
	uk_file_acquire(source->file);
	first = vfs_lookupat(source->file, oldpath + from.pos,
			     from.len - from.pos, VFS_LOOKUP_NOFOLLOW);
	uk_file_acquire(destination->file);
	second = vfs_lookupat(destination->file, newpath + to.pos,
			      to.len - to.pos, VFS_LOOKUP_NOFOLLOW);
	if (PTRISERR(first) || PTRISERR(second) || first->state != second->state)
		move = vfs_path_move_prepare(source, oldpath + from.pos,
					     from.len - from.pos, destination,
					     newpath + to.pos, to.len - to.pos,
					     flags & 2);
	if (!PTRISERR(first))
		uk_file_release(first);
	if (!PTRISERR(second))
		uk_file_release(second);
	if (PTRISERR(move)) {
		result = PTR2ERR(move);
		move = NULL;
		goto out;
	}
	result = vfs_renameat(oldof ? oldof->file : NULL, oldpath,
			      newof ? newof->file : NULL, newpath, flags);
	vfs_path_move_finish(move, !result);
out:
	uk_posix_vfs_path_release(source);
	uk_posix_vfs_path_release(destination);
	uk_mutex_unlock(&vfs_namespace_lock);
	return result;
}
