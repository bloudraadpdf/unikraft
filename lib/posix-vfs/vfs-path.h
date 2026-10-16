/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSIX_VFS_PATH_H
#define POSIX_VFS_PATH_H

#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/vfs-path.h>

struct uk_vfs_path {
	struct uk_list_head link;
	struct uk_vfs_path *parent;
	const struct uk_file *file;
	char *name;
	size_t length;
	unsigned int references;
	bool deleted;
};

extern struct uk_mutex vfs_namespace_lock;

struct uk_vfs_path *vfs_path_get(struct uk_vfs_path *parent,
				 const struct uk_file *file, const char *name, size_t length);
struct uk_fs_path vfs_path_cwd_name(struct uk_vfs_path *path);
struct uk_vfs_path *vfs_path_parent(struct uk_vfs_path *path);
struct uk_vfs_path *vfs_path_find(const struct uk_file *file,
				  const struct uk_fs_path *name);
void vfs_path_unlinked(const struct uk_file *parent,
		       const char *name, size_t length);
struct vfs_path_move;
struct vfs_path_move *vfs_path_move_prepare(struct uk_vfs_path *source,
					    const char *name, size_t length,
					    struct uk_vfs_path *destination,
					    const char *newname, size_t newlength, bool exchange);
void vfs_path_move_finish(struct vfs_path_move *move, bool success);

#endif
