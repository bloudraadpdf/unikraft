/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UKFS_EXT4_INODE_IO_H
#define UKFS_EXT4_INODE_IO_H
#include <stddef.h>
#include <stdint.h>
#include <uk/arch/types.h>
#include <ext4_fs.h>
__u64 ukext4_inode_max_size(struct ext4_sblock *);
int ukext4_inode_read(struct ext4_inode_ref *, void *, size_t, uint64_t, size_t *);
int ukext4_inode_write(struct ext4_inode_ref *, const void *, size_t, uint64_t, size_t *);
int ukext4_inode_resize(struct ext4_inode_ref *, uint64_t);
#endif
