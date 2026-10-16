/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <string.h>
#include <sys/stat.h>

#include <uk/alloc.h>
#include <uk/rwlock.h>
#if CONFIG_LIBPOSIX_VFS
#include <uk/posix-vfs.h>
#endif

#include "unixsock-bind.h"

#define UNIX_BUCKETS 128

static struct unix_addr_entry *hash_tbl[UNIX_BUCKETS];
static struct uk_rwlock hash_lock = UK_RWLOCK_INITIALIZER(hash_lock, 0);

static size_t unix_addr_hash(const char *name, size_t length,
			     const struct uk_file *inode)
{
	size_t hash = 0;

	if (inode)
		return (((uintptr_t)inode->vol ^ (uintptr_t)inode->node) >> 4) %
			UNIX_BUCKETS;
	for (size_t i = 0; i < length; i++)
		hash ^= name[i];
	return hash % UNIX_BUCKETS;
}

static struct unix_addr_entry **unix_addr_get(const char *name, size_t length,
					      const struct uk_file *inode)
{
	struct unix_addr_entry **entry;

	for (entry = &hash_tbl[unix_addr_hash(name, length, inode)];
	     *entry; entry = &(*entry)->next) {
		if (inode) {
			if ((*entry)->inode &&
			    (*entry)->inode->vol == inode->vol &&
			    (*entry)->inode->node == inode->node)
				break;
		} else if (!(*entry)->inode && (*entry)->namelen == length &&
			   !memcmp((*entry)->name, name, length)) {
			break;
		}
	}
	return entry;
}

static const struct uk_file *unix_addr_inode(const char *name, size_t length)
{
#if CONFIG_LIBPOSIX_VFS
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct uk_ofile *opened;
	const struct uk_file *inode;
	struct uk_statx status;
	char *path = uk_malloc(allocator, length + 1);
	int error;

	if (!path)
		return ERR2PTR(-ENOMEM);
	memcpy(path, name, length);
	path[length] = '\0';
	opened = uk_sys_openat(NULL, path, O_PATH, 0);
	uk_free(allocator, path);
	if (PTRISERR(opened))
		return ERR2PTR(PTR2ERR(opened));
	inode = opened->file;
	error = uk_file_getstat(inode, UK_STATX_TYPE, &status);
	if (!error && (status.stx_mode & S_IFMT) != S_IFSOCK)
		error = -ECONNREFUSED;
	if (!error)
		uk_file_acquire(inode);
	uk_ofile_release(opened);
	return error ? ERR2PTR(error) : inode;
#else
	(void)name;
	(void)length;
	return ERR2PTR(-EOPNOTSUPP);
#endif
}

int unix_addr_bind(struct unix_addr_entry *entry)
{
	struct unix_addr_entry **position;
	int error = 0;

	if (entry->name[0]) {
#if CONFIG_LIBPOSIX_VFS
		entry->inode = uk_posix_vfs_socket_create(entry->name);
		if (PTRISERR(entry->inode)) {
			error = PTR2ERR(entry->inode);
			entry->inode = NULL;
			return error == -EEXIST ? -EADDRINUSE : error;
		}
#else
		return -EOPNOTSUPP;
#endif
	}
	uk_rwlock_wlock(&hash_lock);
	position = unix_addr_get(entry->name, entry->namelen, entry->inode);
	if (*position) {
		error = -EADDRINUSE;
	} else {
		uk_file_acquire_weak(entry->sock);
		*position = entry;
	}
	uk_rwlock_wunlock(&hash_lock);
	if (error && entry->inode) {
		uk_file_release(entry->inode);
		entry->inode = NULL;
	}
	return error;
}

posix_sock *unix_addr_lookup(const char *name, size_t length)
{
	const struct uk_file *inode = NULL;
	struct unix_addr_entry **position;
	posix_sock *socket;

	if (!length)
		return ERR2PTR(-EINVAL);
	if (name[0]) {
		inode = unix_addr_inode(name, length);
		if (PTRISERR(inode))
			return (posix_sock *)inode;
	}
	uk_rwlock_rlock(&hash_lock);
	position = unix_addr_get(name, length, inode);
	if (*position) {
		socket = (*position)->sock;
		uk_file_acquire_weak(socket);
	} else {
		socket = ERR2PTR(-ECONNREFUSED);
	}
	uk_rwlock_runlock(&hash_lock);
	if (inode)
		uk_file_release(inode);
	return socket;
}

int unix_addr_release(struct unix_addr_entry *entry)
{
	struct unix_addr_entry **position;
	int error = 0;

	uk_rwlock_wlock(&hash_lock);
	position = unix_addr_get(entry->name, entry->namelen, entry->inode);
	if (*position != entry) {
		error = -ENOENT;
	} else {
		*position = entry->next;
		entry->next = NULL;
	}
	uk_rwlock_wunlock(&hash_lock);
	if (!error) {
		uk_file_release_weak(entry->sock);
		if (entry->inode)
			uk_file_release(entry->inode);
	}
	return error;
}
