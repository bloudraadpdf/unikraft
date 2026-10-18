/* SPDX-License-Identifier: BSD-3-Clause */
#include "inode-io.h"
#include <errno.h>
#include <string.h>
#include <ext4_blockdev.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_trans.h>

__u64 ukext4_inode_max_size(struct ext4_sblock *superblock)
{
	const __u32 block_size = ext4_sb_get_block_size(superblock);
	__u64 blocks = UINT32_MAX;

	if (!ext4_sb_feature_ro_com(superblock, EXT4_FRO_COM_HUGE_FILE))
		blocks /= block_size / 512;
	return blocks * block_size;
}

int ukext4_inode_read(struct ext4_inode_ref *inode, void *output,
                     size_t length, uint64_t offset, size_t *done)
{
    const uint64_t size = ext4_inode_get_size(&inode->fs->sb, inode->inode);
    const uint32_t block_size = ext4_sb_get_block_size(&inode->fs->sb);
    *done = 0;
    if (offset >= size) return 0;
    if (length > size - offset) length = size - offset;
    while (*done < length) {
        struct ext4_block block;
        ext4_fsblk_t physical;
        const size_t within = offset % block_size;
        size_t count = block_size - within;
        if (count > length - *done) count = length - *done;
        int result = ext4_fs_get_inode_dblk_idx(inode, offset / block_size,
                                               &physical, true);
        if (result) return result;
        if (!physical) memset((char *)output + *done, 0, count);
        else {
            result = ext4_block_get(inode->fs->bdev, &block, physical);
            if (result) return result;
            memcpy((char *)output + *done, block.data + within, count);
            result = ext4_block_set(inode->fs->bdev, &block);
            if (result) return result;
        }
        offset += count;
        *done += count;
    }
    return 0;
}

int ukext4_inode_write(struct ext4_inode_ref *inode, const void *input,
                      size_t length, uint64_t offset, size_t *done)
{
    const uint32_t block_size = ext4_sb_get_block_size(&inode->fs->sb);
	const __u64 maximum = ukext4_inode_max_size(&inode->fs->sb);
    *done = 0;
    if (!length) return 0;
	if (offset >= maximum)
		return EFBIG;
	if (length > maximum - offset)
		length = maximum - offset;
    if (!ext4_inode_has_flag(inode->inode, EXT4_INODE_FLAG_EXTENTS))
        return EOPNOTSUPP;
    while (*done < length) {
        struct ext4_block block;
        ext4_fsblk_t physical;
        uint32_t blocks;
        const size_t within = offset % block_size;
        size_t count = block_size - within;
        if (count > length - *done) count = length - *done;
        int result = ext4_extent_get_blocks(inode, offset / block_size, 1,
                                            &physical, false, &blocks);
        if (result) return result;
        const bool hole = !physical;
        if (hole) {
            result = ext4_extent_get_blocks(inode, offset / block_size, 1,
                                            &physical, true, &blocks);
            if (result) return result;
            if (!physical || !blocks) return EIO;
        }
        result = hole ? ext4_trans_block_get_noread(inode->fs->bdev, &block, physical)
                      : ext4_trans_block_get(inode->fs->bdev, &block, physical);
        if (result) return result;
        if (hole) memset(block.data, 0, block_size);
        memcpy(block.data + within, (const char *)input + *done, count);
        result = ext4_trans_set_block_dirty(block.buf);
        int released = ext4_block_set(inode->fs->bdev, &block);
        if (result || released) return result ? result : released;
        offset += count;
        *done += count;
        if (offset > ext4_inode_get_size(&inode->fs->sb, inode->inode)) {
            ext4_inode_set_size(inode->inode, offset);
            inode->dirty = true;
        }
    }
    return 0;
}

int ukext4_inode_resize(struct ext4_inode_ref *inode, uint64_t size)
{
    const uint64_t old_size = ext4_inode_get_size(&inode->fs->sb, inode->inode);
    const uint32_t block_size = ext4_sb_get_block_size(&inode->fs->sb);
	if (size > ukext4_inode_max_size(&inode->fs->sb))
		return EFBIG;
    if (size >= old_size) {
        ext4_inode_set_size(inode->inode, size);
        inode->dirty = true;
        return 0;
    }
    int result = ext4_fs_truncate_inode(inode, size);
    if (result || !(size % block_size)) return result;
    ext4_fsblk_t physical;
    result = ext4_fs_get_inode_dblk_idx(inode, size / block_size, &physical, true);
    if (result || !physical) return result;
    struct ext4_block block;
    result = ext4_trans_block_get(inode->fs->bdev, &block, physical);
    if (result) return result;
    memset(block.data + size % block_size, 0, block_size - size % block_size);
    result = ext4_trans_set_block_dirty(block.buf);
    int released = ext4_block_set(inode->fs->bdev, &block);
    return result ? result : released;
}
