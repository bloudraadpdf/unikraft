/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_VFS_FIFO_H__
#define __UK_VFS_FIFO_H__

#include <uk/file.h>

const struct uk_file *vfs_fifo_open(const struct uk_file *inode,
				    unsigned int mode);

#endif
