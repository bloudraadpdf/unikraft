/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <uk/alloc.h>
#include "vfs-path.h"

struct uk_mutex vfs_namespace_lock = UK_MUTEX_INITIALIZER(vfs_namespace_lock);
static struct uk_mutex path_lock = UK_MUTEX_INITIALIZER(path_lock);
static UK_LIST_HEAD(paths);

struct vfs_path_move {
	struct vfs_path_move *next;
	struct uk_vfs_path *path;
	struct uk_vfs_path *parent;
	char *name;
	size_t length;
	bool remove;
};

static bool path_matches(const struct uk_vfs_path *path,
			 const struct uk_file *parent, const char *name, size_t length)
{
	return !path->deleted && path->parent &&
		path->parent->file->state == parent->state &&
		path->length == length && !memcmp(path->name, name, length);
}

void uk_posix_vfs_path_acquire(struct uk_vfs_path *path)
{
	uk_mutex_lock(&path_lock);
	UK_ASSERT(path->references);
	path->references++;
	uk_mutex_unlock(&path_lock);
}

void uk_posix_vfs_path_release(struct uk_vfs_path *path)
{
	struct uk_list_head dead = UK_LIST_HEAD_INIT(dead);
	struct uk_vfs_path *parent, *next;

	if (!path)
		return;
	uk_mutex_lock(&path_lock);
	while (path && !--path->references) {
		parent = path->parent;
		uk_list_del_init(&path->link);
		uk_list_add_tail(&path->link, &dead);
		path = parent;
	}
	uk_mutex_unlock(&path_lock);
	uk_list_for_each_entry_safe(path, next, &dead, link) {
		uk_file_release(path->file);
		uk_free(uk_alloc_get_default(), path->name);
		uk_free(uk_alloc_get_default(), path);
	}
}

struct uk_vfs_path *vfs_path_get(struct uk_vfs_path *parent,
				 const struct uk_file *file, const char *name, size_t length)
{
	struct uk_vfs_path *path;

	uk_mutex_lock(&path_lock);
	uk_list_for_each_entry(path, &paths, link) {
		if (!path->deleted && path->parent == parent &&
		    path->file->state == file->state && path->length == length &&
		    !memcmp(path->name, name, length)) {
			path->references++;
			goto out;
		}
	}
	path = uk_zalloc(uk_alloc_get_default(), sizeof(*path));
	if (!path)
		goto out;
	path->name = uk_malloc(uk_alloc_get_default(), length + 1);
	if (!path->name) {
		uk_free(uk_alloc_get_default(), path);
		path = NULL;
		goto out;
	}
	memcpy(path->name, name, length);
	path->name[length] = '\0';
	path->length = length;
	path->parent = parent;
	if (parent)
		parent->references++;
	path->file = file;
	uk_file_acquire(file);
	path->references = 1;
	uk_list_add_tail(&path->link, &paths);
out:
	uk_mutex_unlock(&path_lock);
	return path ? path : ERR2PTR(-ENOMEM);
}

struct uk_vfs_path *vfs_path_parent(struct uk_vfs_path *path)
{
	struct uk_vfs_path *parent;

	uk_mutex_lock(&path_lock);
	parent = path->parent ? path->parent : path;
	parent->references++;
	uk_mutex_unlock(&path_lock);
	return parent;
}

const struct uk_file *uk_posix_vfs_path_file(struct uk_vfs_path *path)
{
	uk_file_acquire(path->file);
	return path->file;
}

static struct uk_fs_path path_name_locked(struct uk_vfs_path *path,
					  bool show_deleted)
{
	static const char suffix[] = " (deleted)";
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct uk_vfs_path *position;
	bool deleted = false;
	size_t length = 0, offset;
	char *buffer;

	for (position = path; position->parent; position = position->parent) {
		if (position->length >= PATH_MAX ||
		    length > PATH_MAX - position->length - 1)
			return (struct uk_fs_path){.s = ERR2PTR(-ENAMETOOLONG)};
		length += position->length + 1;
		deleted |= position->deleted;
	}
	if (deleted && !show_deleted)
		return (struct uk_fs_path){.s = ERR2PTR(-ENOENT)};
	if (!length)
		length = 1;
	if (deleted)
		length += sizeof(suffix) - 1;
	buffer = uk_malloc(allocator, length + 1);
	if (!buffer)
		return (struct uk_fs_path){.s = ERR2PTR(-ENOMEM)};
	offset = length;
	buffer[offset] = '\0';
	if (deleted) {
		offset -= sizeof(suffix) - 1;
		memcpy(buffer + offset, suffix, sizeof(suffix) - 1);
	}
	if (!path->parent)
		buffer[--offset] = '/';
	for (position = path; position->parent; position = position->parent) {
		offset -= position->length;
		memcpy(buffer + offset, position->name, position->length);
		buffer[--offset] = '/';
	}
	UK_ASSERT(!offset);
	return (struct uk_fs_path){.s = buffer, .len = length,
				  .allocator = allocator};
}

struct uk_fs_path uk_posix_vfs_path_name(struct uk_vfs_path *path)
{
	struct uk_fs_path name;

	uk_mutex_lock(&path_lock);
	name = path_name_locked(path, true);
	uk_mutex_unlock(&path_lock);
	return name;
}

struct uk_fs_path vfs_path_cwd_name(struct uk_vfs_path *path)
{
	struct uk_fs_path name;

	uk_mutex_lock(&path_lock);
	name = path_name_locked(path, false);
	uk_mutex_unlock(&path_lock);
	return name;
}

struct uk_vfs_path *vfs_path_find(const struct uk_file *file,
				  const struct uk_fs_path *name)
{
	struct uk_vfs_path *path, *found = NULL;
	struct uk_fs_path candidate;

	uk_mutex_lock(&path_lock);
	uk_list_for_each_entry(path, &paths, link) {
		if (path->file->state != file->state)
			continue;
		candidate = path_name_locked(path, true);
		if (PTRISERR(candidate.s))
			continue;
		if (candidate.len == name->len &&
		    !memcmp(candidate.s, name->s, name->len))
			found = path;
		uk_fs_path_release(candidate);
		if (found) {
			found->references++;
			break;
		}
	}
	uk_mutex_unlock(&path_lock);
	return found ? found : ERR2PTR(-ENOENT);
}

void vfs_path_unlinked(const struct uk_file *parent,
		       const char *name, size_t length)
{
	struct uk_vfs_path *path;

	uk_mutex_lock(&path_lock);
	uk_list_for_each_entry(path, &paths, link) {
		if (path_matches(path, parent, name, length))
			path->deleted = true;
	}
	uk_mutex_unlock(&path_lock);
}

struct vfs_path_move *vfs_path_move_prepare(struct uk_vfs_path *source,
					    const char *name, size_t length,
					    struct uk_vfs_path *destination,
					    const char *newname, size_t newlength, bool exchange)
{
	struct vfs_path_move *moves = NULL, *move;
	struct uk_vfs_path *path, *parent;
	const char *target;
	size_t target_length;
	bool remove;

	uk_mutex_lock(&path_lock);
	uk_list_for_each_entry(path, &paths, link) {
		if (path_matches(path, source->file, name, length)) {
			parent = destination;
			target = newname;
			target_length = newlength;
			remove = false;
		} else if (path_matches(path, destination->file,
					newname, newlength)) {
			parent = source;
			target = name;
			target_length = length;
			remove = !exchange;
		} else {
			continue;
		}
		move = uk_zalloc(uk_alloc_get_default(), sizeof(*move));
		if (!move)
			goto no_memory;
		move->name = uk_malloc(uk_alloc_get_default(), target_length + 1);
		if (!move->name) {
			uk_free(uk_alloc_get_default(), move);
			goto no_memory;
		}
		memcpy(move->name, target, target_length);
		move->name[target_length] = '\0';
		move->length = target_length;
		move->path = path;
		path->references++;
		move->parent = parent;
		parent->references++;
		move->remove = remove;
		move->next = moves;
		moves = move;
	}
	uk_mutex_unlock(&path_lock);
	return moves;
no_memory:
	uk_mutex_unlock(&path_lock);
	vfs_path_move_finish(moves, false);
	return ERR2PTR(-ENOMEM);
}

void vfs_path_move_finish(struct vfs_path_move *move, bool success)
{
	struct vfs_path_move *next, *position;
	struct uk_vfs_path *parent;
	char *name;

	if (success) {
		uk_mutex_lock(&path_lock);
		for (position = move; position; position = position->next) {
			if (position->remove) {
				position->path->deleted = true;
				continue;
			}
			parent = position->path->parent;
			name = position->path->name;
			position->path->parent = position->parent;
			position->path->name = position->name;
			position->path->length = position->length;
			position->parent = parent;
			position->name = name;
		}
		uk_mutex_unlock(&path_lock);
	}
	while (move) {
		next = move->next;
		uk_posix_vfs_path_release(move->parent);
		uk_posix_vfs_path_release(move->path);
		uk_free(uk_alloc_get_default(), move->name);
		uk_free(uk_alloc_get_default(), move);
		move = next;
	}
}
