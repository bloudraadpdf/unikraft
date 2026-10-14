/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UKFS_EXT4_FILE_PAGES_H
#define UKFS_EXT4_FILE_PAGES_H
#include <uk/file.h>
#include <uk/sparsebuf.h>
#include "inode-io.h"
struct ukext4_pages {
    struct uk_sparsebuf_blk *head;
};
ssize_t ukext4_pages_mem(struct ukext4_pages *, struct ext4_inode_ref *,
                        enum uk_file_mem_op, size_t, size_t, struct iovec *, size_t);
int ukext4_pages_io(struct ukext4_pages *, struct ext4_inode_ref *,
                    void *, size_t, size_t, size_t *, int);
int ukext4_pages_writeback(struct ukext4_pages *, struct ext4_inode_ref *, int *);
void ukext4_pages_release(struct ukext4_pages *, size_t, size_t, size_t);
void ukext4_pages_zero(struct ukext4_pages *, size_t);
void ukext4_pages_clear(struct ukext4_pages *);
#endif
