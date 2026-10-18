/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_VFS_PATH_H__
#define __UK_VFS_PATH_H__

#include <uk/fs.h>

struct uk_vfs_path;
struct uk_ofile;
struct uk_fs_path uk_posix_vfs_ofile_name(struct uk_ofile *file);
struct uk_vfs_path *uk_posix_vfs_ofile_path(struct uk_ofile *file);

struct uk_ofile *uk_posix_vfs_exec_open(const char *name,
					struct uk_vfs_path **identity);
void uk_posix_vfs_path_acquire(struct uk_vfs_path *path);
void uk_posix_vfs_path_release(struct uk_vfs_path *path);
struct uk_fs_path uk_posix_vfs_path_name(struct uk_vfs_path *path);
const struct uk_file *uk_posix_vfs_path_file(struct uk_vfs_path *path);

#endif
