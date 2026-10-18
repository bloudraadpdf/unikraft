/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <uk/alloc.h>
#include <uk/assert.h>
#include <uk/config.h>
#include <uk/init.h>
#include <uk/mutex.h>
#include <uk/nofault.h>
#include <uk/syscall.h>

#include <uk/posix-fdtab.h>

#include "fmap.h"

#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
#include <uk/essentials.h>
#include <uk/refcount.h>
#include <uk/thread.h>
#include <uk/spinlock.h>
#include <uk/lcpu.h>
#endif /* CONFIG_LIBPOSIX_FDTAB_MULTITAB */

#if CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM
#include <uk/posix-fdtab-legacy.h>
#endif /* CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM */

#if CONFIG_LIBPOSIX_PROCESS
#include <uk/process.h>
#endif /* CONFIG_LIBPOSIX_PROCESS */

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
#include <uk/event.h>
#include <uk/prio.h>
#endif /* CONFIG_LIBPOSIX_PROCESS_EXECVE */

#define UK_FDTAB_SIZE CONFIG_LIBPOSIX_FDTAB_MAXFDS
UK_CTASSERT(UK_FDTAB_SIZE <= UK_FD_MAX);


/* Under mutation_lock, a used NULL map entry is an unpublished reservation. */
struct uk_fdtab {
	struct uk_alloc *alloc;
	struct uk_mutex mutation_lock;
	struct uk_fmap fmap;
#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
	__atomic refcnt;
#endif /* CONFIG_LIBPOSIX_FDTAB_MULTITAB */
	unsigned long _bmap[UK_BMAP_NELEM(UK_FDTAB_SIZE)];
	void *_fdmap[UK_FDTAB_SIZE];
	unsigned long reserved_cloexec[UK_BMAP_NELEM(UK_FDTAB_SIZE)];
};

static struct uk_fdtab init_fdtab = {
	.mutation_lock = UK_MUTEX_INITIALIZER(init_fdtab.mutation_lock),
	.fmap = {
		.bmap = {
			.size = UK_FDTAB_SIZE,
			.bitmap = init_fdtab._bmap
		},
		.map = init_fdtab._fdmap
	},
#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
	.refcnt = UK_REFCOUNT_INITIALIZER(1)
#endif /* CONFIG_LIBPOSIX_FDTAB_MULTITAB */
};

#if CONFIG_LIBPOSIX_FDTAB_MULTITAB

/* Every thread keeps its own fdtab reference */
static __uk_tls struct uk_fdtab *active_fdtab;
static uk_spinlock fdtab_thread_lock = UK_SPINLOCK_INITIALIZER();

#else /* !CONFIG_LIBPOSIX_FDTAB_MULTITAB */

/* All threads share the same static init fdtab */
static struct uk_fdtab *const active_fdtab = &init_fdtab;

#endif /* !CONFIG_LIBPOSIX_FDTAB_MULTITAB */

const void *uk_fdtab_lock_owner(void)
{
	return active_fdtab;
}

static int init_posix_fdtab(struct uk_init_ctx *ictx __unused)
{
	init_fdtab.alloc = uk_alloc_get_default();
	/* Consider skipping init for .map (static vars are inited to 0) */
	uk_fmap_init(&init_fdtab.fmap);
	memset(init_fdtab.reserved_cloexec, 0,
	       sizeof(init_fdtab.reserved_cloexec));
#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
	/* Ensure the init thread has a valid fdtab ref */
	uk_refcount_acquire(&init_fdtab.refcnt);
	active_fdtab = &init_fdtab;
#endif /* CONFIG_LIBPOSIX_FDTAB_MULTITAB */
	return 0;
}

/* Encode flags in entry pointer using the least significant bits */
/* made available by the open file structure's alignment */
struct fdval {
	void *p;
	int flags;
};

#define UK_FDTAB_CLOEXEC 1

#if CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM
#define UK_FDTAB_VFSCORE 2
#define _MAX_FLAG 2
#else /* !CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM */
#define _MAX_FLAG 1
#endif /* !CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM */

#define _FLAG_MASK (((uintptr_t)_MAX_FLAG << 1) - 1)

UK_CTASSERT(__alignof__(struct uk_ofile) > _MAX_FLAG);
#if CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM && CONFIG_LIBVFSCORE
UK_CTASSERT(__alignof__(struct vfscore_file) > _MAX_FLAG);
#endif /* CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM && CONFIG_LIBVFSCORE */

static inline const void *fdtab_encode(const void *f, int flags)
{
	UK_ASSERT(!((uintptr_t)f & _FLAG_MASK));
	return (const void *)((uintptr_t)f | flags);
}

static inline struct fdval fdtab_decode(void *p)
{
	uintptr_t v = (uintptr_t)p;

	return (struct fdval) {
		.p = (void *)(v & ~_FLAG_MASK),
		.flags = v & _FLAG_MASK
	};
}

static inline void file_acq(void *p, int flags __maybe_unused)
{
#if CONFIG_LIBVFSCORE
	if (flags & UK_FDTAB_VFSCORE)
		fhold((struct vfscore_file *)p);
	else
#endif /* CONFIG_LIBVFSCORE */
		uk_ofile_acquire((struct uk_ofile *)p);
}

static inline void file_rel(void *p, int flags __maybe_unused)
{
#if CONFIG_LIBVFSCORE
	if (flags & UK_FDTAB_VFSCORE)
		fdrop((struct vfscore_file *)p);
	else
#endif /* CONFIG_LIBVFSCORE */
		uk_ofile_release((struct uk_ofile *)p);
}

static void file_close(struct uk_fdtab *tab, struct fdval value)
{
#if CONFIG_LIBVFSCORE
	if (!(value.flags & UK_FDTAB_VFSCORE))
#endif
		uk_file_lock_release(((struct uk_ofile *)value.p)->file,
				     tab, UK_FILE_LOCK_PROCESS);
	file_rel(value.p, value.flags);
}

static void *fdtab_entry_locked(struct uk_fdtab *table, int fd)
{
	return fd >= 0 && fd < UK_FDTAB_SIZE ? table->fmap.map[fd] : NULL;
}

static struct fdval fdtab_get_locked(struct uk_fdtab *table, int fd)
{
	void *entry = fdtab_entry_locked(table, fd);
	struct fdval value = {0};

	if (entry) {
		value = fdtab_decode(entry);
		file_acq(value.p, value.flags);
	}
	return value;
}

static struct fdval _fdtab_get(struct uk_fdtab *table, int fd)
{
	struct fdval value;

	uk_mutex_lock(&table->mutation_lock);
	value = fdtab_get_locked(table, fd);
	uk_mutex_unlock(&table->mutation_lock);
	return value;
}

static int fdtab_close(struct uk_fdtab *tab, int fd, bool cloexec)
{
	void *entry;

	uk_mutex_lock(&tab->mutation_lock);
	entry = fdtab_entry_locked(tab, fd);
	if (entry && cloexec &&
	    !(fdtab_decode(entry).flags & UK_FDTAB_CLOEXEC))
		entry = NULL;
	if (entry)
		entry = uk_fmap_take(&tab->fmap, fd);
	uk_mutex_unlock(&tab->mutation_lock);
	if (!entry)
		return -EBADF;
	file_close(tab, fdtab_decode(entry));
	return 0;
}

static int fdtab_setflags_locked(struct uk_fdtab *tab, int fd,
				 int set, int clear)
{
	void *entry = fdtab_entry_locked(tab, fd);
	struct fdval value;

	if (!entry)
		return -EBADF;
	value = fdtab_decode(entry);
	value.flags = (value.flags & ~clear) | set;
	uk_fmap_set(&tab->fmap, fd, fdtab_encode(value.p, value.flags));
	return 0;
}

static int fdtab_setflags(struct uk_fdtab *tab, int fd, int set, int clear)
{
	int result;

	uk_mutex_lock(&tab->mutation_lock);
	result = fdtab_setflags_locked(tab, fd, set, clear);
	uk_mutex_unlock(&tab->mutation_lock);
	return result;
}

static unsigned int fdtab_limit(void)
{
#if CONFIG_LIBPOSIX_PROCESS
	return MIN(uk_posix_nofile_limit(), UK_FDTAB_SIZE);
#else
	return UK_FDTAB_SIZE;
#endif
}

static int fdtab_put(const void *entry, int min)
{
	struct uk_fmap map = active_fdtab->fmap;
	int fd;

	map.bmap.size = fdtab_limit();
	uk_mutex_lock(&active_fdtab->mutation_lock);
	fd = uk_fmap_put(&map, entry, min);
	uk_mutex_unlock(&active_fdtab->mutation_lock);
	return fd < (int)map.bmap.size ? fd : -EMFILE;
}

/* Ops */

int uk_fdtab_open_desc(struct uk_ofile *of, unsigned int mode)
{
	int fd;
	const void *entry = fdtab_encode(of,
		(mode & O_CLOEXEC) ? UK_FDTAB_CLOEXEC : 0);

	fd = fdtab_put(entry, 0);
	return fd;
}

int uk_fdtab_open_named(const struct uk_file *f, unsigned int mode,
			const char *name, size_t len)
{
	struct uk_ofile *of;
	int fd;

	UK_ASSERT(f);
	if (!name)
		len = 0;

	of = uk_ofile_new(f, mode, len);
	if (unlikely(!of))
		return -ENOMEM;
	if (len) {
		memcpy(of->name, name, len);
		of->name[len] = 0;
	}

	/* Place the file in fdtab */
	fd = uk_fdtab_open_desc(of, mode);
	if (unlikely(fd < 0))
		uk_ofile_release(of);
	return fd;
}

int uk_fdtab_open(const struct uk_file *f, unsigned int mode)
{
	return uk_fdtab_open_named(f, mode, NULL, 0);
}

int uk_fdtab_setflags(int fd, int flags)
{
	if (flags & ~O_CLOEXEC)
		return -EINVAL;
	return fdtab_setflags(active_fdtab, fd,
			      flags ? UK_FDTAB_CLOEXEC : 0, UK_FDTAB_CLOEXEC);
}

int uk_fdtab_getflags(int fd)
{
	void *entry;
	int result = -EBADF;

	uk_mutex_lock(&active_fdtab->mutation_lock);
	entry = fdtab_entry_locked(active_fdtab, fd);
	if (entry)
		result = (fdtab_decode(entry).flags & UK_FDTAB_CLOEXEC) ?
			 O_CLOEXEC : 0;
	uk_mutex_unlock(&active_fdtab->mutation_lock);
	return result;
}

#if CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM
#if CONFIG_LIBVFSCORE
int uk_fdtab_legacy_open(struct vfscore_file *vf)
{
	const void *entry;
	int fd;

	fhold(vf);
	entry = fdtab_encode(vf, UK_FDTAB_VFSCORE);
	fd = fdtab_put(entry, 0);
	if (fd < 0)
		goto err_out;
	vf->fd = fd;
	return fd;
err_out:
	fdrop(vf);
	return fd;
}

struct vfscore_file *uk_fdtab_legacy_get(int fd)
{
	struct fdval value = _fdtab_get(active_fdtab, fd);

	if (value.p && !(value.flags & UK_FDTAB_VFSCORE)) {
		file_rel(value.p, value.flags);
		return NULL;
	}
	return value.p;
}

#endif /* CONFIG_LIBVFSCORE */

int uk_fdtab_shim_get(int fd, union uk_shim_file *out)
{
	struct fdval value = _fdtab_get(active_fdtab, fd);

	if (!value.p)
		return -1;
#if CONFIG_LIBVFSCORE
	if (value.flags & UK_FDTAB_VFSCORE) {
		out->vfile = value.p;
		return UK_SHIM_LEGACY;
	}
#endif
	out->ofile = value.p;
	return UK_SHIM_OFILE;
}

#endif /* CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM */


struct uk_ofile *uk_fdtab_get_from(struct uk_fdtab *table, int fd)
{
	struct fdval v = _fdtab_get(table, fd);

#if CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM
	/* Report legacy files as not present if called through new API */
	if (v.p && v.flags & UK_FDTAB_VFSCORE) {
		file_rel(v.p, v.flags);
		return NULL;
	}
#endif /* CONFIG_LIBPOSIX_FDTAB_LEGACY_SHIM */
	return (struct uk_ofile *)v.p;
}

struct uk_ofile *uk_fdtab_get(int fd)
{
	return uk_fdtab_get_from(active_fdtab, fd);
}

int uk_fdtab_next(struct uk_fdtab *table, int start)
{
	int result = -1;

	uk_mutex_lock(&table->mutation_lock);
	for (int fd = MAX(start, 0); fd < UK_FDTAB_SIZE; fd++) {
		if (fdtab_entry_locked(table, fd)) {
			result = fd;
			break;
		}
	}
	uk_mutex_unlock(&table->mutation_lock);
	return result;
}

#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
static void fdtab_free(struct uk_fdtab *tab);

struct uk_fdtab *uk_fdtab_get_thread(struct uk_thread *thread)
{
	struct uk_fdtab *table;
	unsigned long flags = uk_lcpu_save_irqf();

	uk_spin_lock(&fdtab_thread_lock);
	table = uk_thread_uktls_var(thread, active_fdtab);
	if (table)
		uk_refcount_acquire(&table->refcnt);
	uk_spin_unlock(&fdtab_thread_lock);
	uk_lcpu_restore_irqf(flags);
	return table;
}

static struct uk_fdtab *fdtab_exchange_thread(struct uk_thread *thread,
					      struct uk_fdtab *table)
{
	struct uk_fdtab *previous;
	unsigned long flags = uk_lcpu_save_irqf();

	uk_spin_lock(&fdtab_thread_lock);
	previous = uk_thread_uktls_var(thread, active_fdtab);
	uk_thread_uktls_var(thread, active_fdtab) = table;
	uk_spin_unlock(&fdtab_thread_lock);
	uk_lcpu_restore_irqf(flags);
	return previous;
}
#endif

void uk_fdtab_put(struct uk_fdtab *table)
{
#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
	if (uk_refcount_release(&table->refcnt))
		fdtab_free(table);
#else
	(void)table;
#endif
}

#if !CONFIG_LIBPOSIX_FDTAB_MULTITAB
struct uk_fdtab *uk_fdtab_get_thread(struct uk_thread *thread __unused)
{
	return active_fdtab;
}
#endif

enum fdtab_reservation_state {
	FDTAB_RESERVED,
	FDTAB_INSTALLED
};

struct uk_fdtab_reservation {
	struct uk_fdtab_reservation *next;
	struct uk_fdtab *table;
	enum fdtab_reservation_state state;
	int fd;
};

static __uk_tls struct uk_fdtab_reservation *fdtab_reservations;

static void fdtab_reservation_unlink(struct uk_fdtab_reservation *reservation)
{
	struct uk_fdtab_reservation **position = &fdtab_reservations;

	while (*position && *position != reservation)
		position = &(*position)->next;
	UK_ASSERT(*position == reservation);
	*position = reservation->next;
}

static void fdtab_reservation_release(struct uk_fdtab_reservation *reservation)
{
	uk_fdtab_put(reservation->table);
	uk_free(uk_alloc_get_default(), reservation);
}

static void fdtab_reservation_cancel(struct uk_fdtab_reservation *reservation)
{
	struct uk_fdtab *table = reservation->table;
	int result __maybe_unused;

	if (reservation->state == FDTAB_RESERVED) {
		uk_mutex_lock(&table->mutation_lock);
		UK_ASSERT(!fdtab_entry_locked(table, reservation->fd));
		uk_clear_bit(reservation->fd, table->reserved_cloexec);
		result = uk_bmap_free(&table->fmap.bmap, reservation->fd);
		UK_ASSERT(!result);
		uk_mutex_unlock(&table->mutation_lock);
	}
	fdtab_reservation_release(reservation);
}

void uk_fdtab_cancel(struct uk_fdtab_reservation *reservation)
{
	if (reservation) {
		fdtab_reservation_unlink(reservation);
		fdtab_reservation_cancel(reservation);
	}
}

struct uk_fdtab_reservation *uk_fdtab_reserve(unsigned int mode)
{
	struct uk_fdtab_reservation *reservation;
	struct uk_fdtab *table = uk_fdtab_get_thread(uk_thread_current());
	unsigned int limit = fdtab_limit();
	struct uk_bmap bitmap;
	size_t available;
	int fd;

	if (!table)
		return ERR2PTR(-EBADF);
	uk_mutex_lock(&table->mutation_lock);
	available = uk_find_next_bit((unsigned long *)table->fmap.bmap.bitmap,
				     limit, 0);
	uk_mutex_unlock(&table->mutation_lock);
	if (available == limit) {
		uk_fdtab_put(table);
		return ERR2PTR(-EMFILE);
	}
	reservation = uk_malloc(uk_alloc_get_default(), sizeof(*reservation));
	if (!reservation) {
		uk_fdtab_put(table);
		return ERR2PTR(-ENOMEM);
	}
	bitmap = table->fmap.bmap;
	bitmap.size = limit;
	uk_mutex_lock(&table->mutation_lock);
	fd = uk_bmap_request(&bitmap, 0);
	if (fd < (int)limit) {
		*reservation = (struct uk_fdtab_reservation){
			.next = fdtab_reservations,
			.table = table,
			.state = FDTAB_RESERVED,
			.fd = fd
		};
		if (mode & O_CLOEXEC)
			uk_set_bit(fd, table->reserved_cloexec);
		else
			uk_clear_bit(fd, table->reserved_cloexec);
		fdtab_reservations = reservation;
	}
	uk_mutex_unlock(&table->mutation_lock);
	if (fd >= (int)limit) {
		uk_free(uk_alloc_get_default(), reservation);
		uk_fdtab_put(table);
		return ERR2PTR(-EMFILE);
	}
	return reservation;
}

int uk_fdtab_install(struct uk_fdtab_reservation *reservation,
		     struct uk_ofile *ofile, int *user_fd)
{
	struct uk_fdtab *table = reservation->table;
	unsigned int flags;
	int result = -EINTR;

	UK_ASSERT(reservation->state == FDTAB_RESERVED);
	if (user_fd && !uk_nofault_try_write(reservation->fd, user_fd)) {
		uk_fdtab_cancel(reservation);
		return -EFAULT;
	}
	uk_mutex_lock(&table->mutation_lock);
	if (table == active_fdtab) {
		UK_ASSERT(!fdtab_entry_locked(table, reservation->fd));
		UK_ASSERT(!uk_bmap_isfree(&table->fmap.bmap, reservation->fd));
		flags = uk_test_bit(reservation->fd, table->reserved_cloexec)
			    ? UK_FDTAB_CLOEXEC
			    : 0;
		uk_fmap_set(&table->fmap, reservation->fd,
			    fdtab_encode(ofile, flags));
		uk_clear_bit(reservation->fd, table->reserved_cloexec);
		reservation->state = FDTAB_INSTALLED;
		result = reservation->fd;
	}
	uk_mutex_unlock(&table->mutation_lock);
	uk_fdtab_cancel(reservation);
	return result;
}

static void fdtab_reservations_cancel_thread(struct uk_thread *thread)
{
	struct uk_fdtab_reservation *reservation =
		uk_thread_uktls_var(thread, fdtab_reservations);

	uk_thread_uktls_var(thread, fdtab_reservations) = NULL;
	while (reservation) {
		struct uk_fdtab_reservation *next = reservation->next;

		fdtab_reservation_cancel(reservation);
		reservation = next;
	}
}

void uk_fdtab_execve_commit(struct uk_thread *thread)
{
	fdtab_reservations_cancel_thread(thread);
}

static int fdtab_reservations_init(struct uk_thread *child,
				   struct uk_thread *parent __unused)
{
	uk_thread_uktls_var(child, fdtab_reservations) = NULL;
	return 0;
}

UK_THREAD_INIT_PRIO_FLAGS(fdtab_reservations_init,
			  fdtab_reservations_cancel_thread, UK_PRIO_LATEST,
			  UK_THREAD_INITF_UKTLS);

static void fdtab_cleanup(struct uk_fdtab *tab, int all)
{
	for (int fd = 0; fd < UK_FDTAB_SIZE; fd++)
		fdtab_close(tab, fd, !all);
}

void uk_fdtab_cloexec(void)
{
	fdtab_cleanup(active_fdtab, 0);
}

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
#if CONFIG_LIBPOSIX_FDTAB_MULTITAB
static struct uk_fdtab *fdtab_duplicate(struct uk_fdtab *tab);
static void fdtab_free(struct uk_fdtab *tab);

static int fdtab_handle_execve(void *data)
{
	struct posix_process_execve_event_data *edat = data;
	struct uk_fdtab *tab = uk_thread_uktls_var(edat->thread, active_fdtab);

	if (uk_refcount_read(&tab->refcnt) > 1) {
		struct uk_fdtab *copy = fdtab_duplicate(tab);
		if (!copy)
			return -ENOMEM;
		fdtab_exchange_thread(edat->thread, copy);
		uk_fdtab_put(tab);
		tab = copy;
	}
	fdtab_cleanup(tab, 0);
	return UK_EVENT_HANDLED_CONT;
}
#else /* !CONFIG_LIBPOSIX_FDTAB_MULTITAB */
static int fdtab_handle_execve(void *data)
{
	(void)data;
	uk_fdtab_cloexec();
	return UK_EVENT_HANDLED_CONT;
}
#endif /* !CONFIG_LIBPOSIX_FDTAB_MULTITAB */

POSIX_PROCESS_EXECVE_HANDLER_PRIO(fdtab_handle_execve, UK_PRIO_EARLIEST);
#endif /* CONFIG_LIBPOSIX_PROCESS_EXECVE */

/* Cleanup all leftover open fds in the initial fdtab */
static void term_posix_fdtab(struct uk_term_ctx *tctx __unused)
{
	fdtab_cleanup(&init_fdtab, 1);
}

#if CONFIG_LIBPOSIX_FDTAB_MULTITAB

/* When using multi-fdtabs, the init thread has a ref to the init fdtab.
 *
 * Newly-created raw threads will start off with a copy of their parent's ref,
 * as a compatibility stop-gap.
 * It is the responsibility of other posix libs and their callbacks to init
 * a new thread's fdtab ref, unsharing as necessary (e.g., clone handler either
 * copying a ref or duplicating the entire fdtab).
 */

/**
 * Duplicate the current fdtab and return a pointer to the copy.
 *
 * @param tab fdtab to duplicate
 *
 * @return
 *  != NULL: Success, pointer to copy
 *  == NULL: Failed to allocate memory
 */
static struct uk_fdtab *fdtab_duplicate(struct uk_fdtab *tab)
{
	struct uk_fdtab *ret = uk_malloc(tab->alloc, sizeof(*ret));

	if (unlikely(!ret))
		return NULL;

	ret->alloc = tab->alloc;
	uk_mutex_init(&ret->mutation_lock);
	ret->fmap = (struct uk_fmap){
		.bmap = {
			.size = UK_FDTAB_SIZE,
			.bitmap = (unsigned long *)ret->_bmap
		},
		.map = ret->_fdmap
	};
	uk_refcount_init(&ret->refcnt, 1);
	uk_fmap_init(&ret->fmap);
	memset(ret->reserved_cloexec, 0, sizeof(ret->reserved_cloexec));
	uk_mutex_lock(&tab->mutation_lock);
	for (int i = 0; i < UK_FDTAB_SIZE; i++) {
		struct fdval v = fdtab_get_locked(tab, i);
		const void *entry = NULL;

		if (v.p)
			entry = fdtab_encode(v.p, v.flags);
		uk_fmap_set(&ret->fmap, i, entry);
	}
	uk_mutex_unlock(&tab->mutation_lock);
	return ret;
}

static void fdtab_free(struct uk_fdtab *tab)
{
	fdtab_cleanup(tab, 1);
	uk_free(tab->alloc, tab);
}

static int fdtab_thread_init(struct uk_thread *child,
			     struct uk_thread *parent)
{
	struct uk_fdtab *tab;

	if (!parent) {
		tab = &init_fdtab;
		uk_refcount_acquire(&tab->refcnt);
	} else {
		tab = uk_fdtab_get_thread(parent);
	}
	fdtab_exchange_thread(child, tab);
	return 0;
}

static void fdtab_thread_term(struct uk_thread *child)
{
	struct uk_fdtab *tab = fdtab_exchange_thread(child, NULL);

	if (tab)
		uk_fdtab_put(tab);
}

UK_THREAD_INIT(fdtab_thread_init, fdtab_thread_term);

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
static int fdtab_clone(void *arg)
{
	struct posix_process_clone_event_data *event_data;
	struct uk_fdtab *tab;
	struct uk_fdtab *newtab;
	const struct clone_args *cl_args;
	struct uk_thread *child;
	struct uk_thread *parent;

	event_data = (struct posix_process_clone_event_data *)arg;
	UK_ASSERT(event_data);

	cl_args = event_data->cl_args;
	UK_ASSERT(cl_args);

	child = event_data->child;
	parent = event_data->parent;

	if (!parent)
		return UK_EVENT_HANDLED_CONT;

	tab = uk_thread_uktls_var(parent, active_fdtab);
	UK_ASSERT(tab); /* Do not call clone from raw threads */

	if (cl_args->flags & CLONE_FILES) {
		/* Inherit parent's fdtab */
		/* As a compat stop-gap, the raw thread already inherited the
		 * parent's fdtab ref; we don't need to do anything.
		 *
		 * TODO: move inheritance here once stopgap is removed.
		 */
		UK_ASSERT(uk_thread_uktls_var(child, active_fdtab) == tab);
		return UK_EVENT_HANDLED_CONT;
	} else {
		/* Duplicate parent's fdtab */
		int r __maybe_unused;

		newtab = fdtab_duplicate(tab);
		if (unlikely(!newtab))
			return -ENOMEM;
		/* Compat stop-gap: release previous duplicate ref */
		UK_ASSERT(uk_thread_uktls_var(child, active_fdtab) == tab);
		fdtab_exchange_thread(child, newtab);
		r = uk_refcount_release(&tab->refcnt);
		UK_ASSERT(!r); /* Cannot have been the last ref */
	}
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_CLONE_HANDLER(CLONE_FILES, fdtab_clone);

#endif /* CONFIG_LIBPOSIX_PROCESS_MULTITHREADING */
#endif /* CONFIG_LIBPOSIX_FDTAB_MULTITAB */

/* Init fdtab as early as possible, to enable functions that rely on fds */
uk_lib_initcall_prio(init_posix_fdtab, 0x0, UK_LIBPOSIX_FDTAB_INIT_PRIO);
/* Place fd cleanup to run latest before any rootfs terminators */
uk_rootfs_initcall_prio(0x0, term_posix_fdtab, UK_PRIO_LATEST);

/* Internal Syscalls */

int uk_sys_close(int fd)
{
	return fdtab_close(active_fdtab, fd, false);
}

int uk_sys_close_range(unsigned int first, unsigned int last,
		       unsigned int flags)
{
	struct uk_fdtab *tab = active_fdtab;
	struct uk_fdtab *previous = NULL;
	unsigned int end = MIN(last, UK_FDTAB_SIZE - 1);

	if (first > last || flags & ~(UK_CLOSE_RANGE_UNSHARE |
				     UK_CLOSE_RANGE_CLOEXEC))
		return -EINVAL;
	if ((flags & UK_CLOSE_RANGE_UNSHARE) &&
	    uk_refcount_read(&tab->refcnt) > 1) {
		struct uk_fdtab *copy = fdtab_duplicate(tab);

		if (!copy)
			return -ENOMEM;
		previous = tab;
		tab = copy;
	}
	if (flags & UK_CLOSE_RANGE_CLOEXEC) {
		uk_mutex_lock(&tab->mutation_lock);
		for (unsigned int fd = first; fd <= end; fd++) {
			if (fdtab_entry_locked(tab, fd))
				fdtab_setflags_locked(tab, fd, UK_FDTAB_CLOEXEC,
						      0);
			else if (!uk_bmap_isfree(&tab->fmap.bmap, fd))
				uk_set_bit(fd, tab->reserved_cloexec);
		}
		uk_mutex_unlock(&tab->mutation_lock);
	} else {
		for (unsigned int fd = first; fd <= end; fd++)
			fdtab_close(tab, fd, false);
	}
	if (previous) {
		fdtab_exchange_thread(uk_thread_current(), tab);
		uk_fdtab_put(previous);
	}
	return 0;
}

int uk_sys_dup3(int oldfd, int newfd, int flags)
{
	int r;
	struct fdval dup;
	void *prevp;
	const void *newent;

	if (oldfd == newfd)
		return -EINVAL;
	if (oldfd < 0 || oldfd >= UK_FDTAB_SIZE ||
	    newfd < 0 || (unsigned int)newfd >= fdtab_limit())
		return -EBADF;
	if (flags & ~O_CLOEXEC)
		return -EINVAL;

	dup = _fdtab_get(active_fdtab, oldfd);
	if (!dup.p)
		return -EBADF; /* oldfd not open */
	dup.flags &= ~UK_FDTAB_CLOEXEC;
	dup.flags |= flags ? UK_FDTAB_CLOEXEC : 0;

	prevp = NULL;
	newent = fdtab_encode(dup.p, dup.flags);
	uk_mutex_lock(&active_fdtab->mutation_lock);
	if (!fdtab_entry_locked(active_fdtab, newfd) &&
	    !uk_bmap_isfree(&active_fdtab->fmap.bmap, newfd))
		r = -EBUSY;
	else
		r = uk_fmap_xchg(&active_fdtab->fmap, newfd, newent, &prevp);
	uk_mutex_unlock(&active_fdtab->mutation_lock);
	if (r) {
		file_rel(dup.p, dup.flags);
		return r;
	}
	if (prevp) {
		struct fdval prevv = fdtab_decode(prevp);

		file_close(active_fdtab, prevv);
	}
	return newfd;
}

int uk_sys_dup2(int oldfd, int newfd)
{
	if (oldfd == newfd)
		if (uk_fdtab_getflags(oldfd) >= 0)
			return newfd;
		else
			return -EBADF;
	else
		return uk_sys_dup3(oldfd, newfd, 0);
}

static int fdtab_dup(int oldfd, int min, int flags, bool check_min)
{
	struct fdval dup;
	const void *newent;
	int fd;

	if (oldfd < 0)
		return -EBADF;
	if (flags & ~O_CLOEXEC)
		return -EINVAL;

	dup = _fdtab_get(active_fdtab, oldfd);
	if (!dup.p)
		return -EBADF;
	if (check_min && (min < 0 || (unsigned int)min >= fdtab_limit())) {
		file_rel(dup.p, dup.flags);
		return -EINVAL;
	}
	dup.flags &= ~UK_FDTAB_CLOEXEC;
	dup.flags |= flags ? UK_FDTAB_CLOEXEC : 0;

	newent = fdtab_encode(dup.p, dup.flags);
	fd = fdtab_put(newent, min);
	if (fd < 0) {
		file_rel(dup.p, dup.flags);
		return fd;
	}
	return fd;
}

int uk_sys_dup_min(int oldfd, int min, int flags)
{
	return fdtab_dup(oldfd, min, flags, true);
}

int uk_sys_dup(int oldfd)
{
	return fdtab_dup(oldfd, 0, 0, false);
}

/* Userspace Syscalls */

UK_SYSCALL_R_DEFINE(int, close, int, fd)
{
	return uk_sys_close(fd);
}

UK_SYSCALL_R_DEFINE(int, close_range, unsigned int, first, unsigned int, last,
		    unsigned int, flags)
{
	return uk_sys_close_range(first, last, flags);
}

UK_SYSCALL_R_DEFINE(int, dup, int, oldfd)
{
	return uk_sys_dup(oldfd);
}

UK_SYSCALL_R_DEFINE(int, dup2, int, oldfd, int, newfd)
{
	return uk_sys_dup2(oldfd, newfd);
}

UK_SYSCALL_R_DEFINE(int, dup3, int, oldfd, int, newfd, int, flags)
{
	return uk_sys_dup3(oldfd, newfd, flags);
}
