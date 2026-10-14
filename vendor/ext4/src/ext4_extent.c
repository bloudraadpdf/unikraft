/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2010 Zheng Liu <lz@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ext4_balloc.h>
#include <ext4_blockdev.h>
#include <ext4_crc32.h>
#include <ext4_extent.h>
#include <ext4_extent_disk.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_trans.h>

struct extent_node {
	struct ext4_block block;
	struct ext4_extent_header *header;
	bool root;
};

struct extent_path {
	struct extent_node nodes[EXT4_EXT_DEPTH_MAX + 1];
	unsigned int slots[EXT4_EXT_DEPTH_MAX];
	unsigned int count;
};

static unsigned int node_count(const struct extent_node *node)
{
	return to_le16(node->header->eh_ecount);
}

static void node_set_count(struct extent_node *node, unsigned int count)
{
	node->header->eh_ecount = to_le16(count);
}

static unsigned int node_capacity(struct ext4_inode_ref *inode, bool root)
{
	size_t size = root ? sizeof(inode->inode->blocks)
			   : ext4_sb_get_block_size(&inode->fs->sb);

	return (size - sizeof(struct ext4_extent_header)) /
	       sizeof(struct ext4_extent);
}

static struct ext4_extent *node_extents(struct extent_node *node)
{
	return (struct ext4_extent *)(node->header + 1);
}

static struct ext4_extent_index *node_indexes(struct extent_node *node)
{
	return (struct ext4_extent_index *)(node->header + 1);
}

static uint32_t node_first(struct extent_node *node)
{
	return to_le32(node_extents(node)->e_blk);
}

static uint32_t extent_length(const struct ext4_extent *extent)
{
	uint32_t length = to_le16(extent->e_len);

	return length > EXT_INIT_MAX_LEN ? length - EXT_INIT_MAX_LEN : length;
}

static bool extent_unwritten(const struct ext4_extent *extent)
{
	return to_le16(extent->e_len) > EXT_INIT_MAX_LEN;
}

static uint64_t extent_block(const struct ext4_extent *extent)
{
	return to_le32(extent->e_start_lo) |
	       ((uint64_t)to_le16(extent->e_start_hi) << 32);
}

static uint64_t index_block(const struct ext4_extent_index *index)
{
	return to_le32(index->ei_leaf_lo) |
	       ((uint64_t)to_le16(index->ei_leaf_hi) << 32);
}

static void index_set(struct ext4_extent_index *index, uint32_t logical,
		      uint64_t physical)
{
	index->ei_blk = to_le32(logical);
	index->ei_leaf_lo = to_le32(physical);
	index->ei_leaf_hi = to_le16(physical >> 32);
	index->ei_unused = 0;
}

static struct ext4_extent extent_make(uint32_t logical, uint64_t physical,
				      uint32_t length, bool unwritten)
{
	struct ext4_extent extent = {
	    .e_blk = to_le32(logical),
	    .e_len = to_le16(length + (unwritten ? EXT_INIT_MAX_LEN : 0)),
	    .e_start_lo = to_le32(physical),
	    .e_start_hi = to_le16(physical >> 32),
	};

	return extent;
}

static bool block_valid(struct ext4_inode_ref *inode, uint64_t block,
			uint32_t length)
{
	uint64_t limit = ext4_sb_get_blocks_cnt(&inode->fs->sb);

	return length && block > ext4_get32(&inode->fs->sb, first_data_block) &&
	       block < limit && length <= limit - block;
}

static size_t checksum_offset(struct extent_node *node)
{
	return sizeof(*node->header) +
	       to_le16(node->header->eh_max) * sizeof(struct ext4_extent);
}

static uint32_t node_checksum(struct ext4_inode_ref *inode,
			      struct extent_node *node)
{
	uint32_t number = to_le32(inode->index);
	uint32_t generation = to_le32(ext4_inode_get_generation(inode->inode));
	uint32_t crc = ext4_crc32c(EXT4_CRC32_INIT, inode->fs->sb.uuid,
				   sizeof(inode->fs->sb.uuid));

	crc = ext4_crc32c(crc, &number, sizeof(number));
	crc = ext4_crc32c(crc, &generation, sizeof(generation));
	return ext4_crc32c(crc, node->header, checksum_offset(node));
}

static bool checksum_enabled(struct ext4_inode_ref *inode)
{
	return ext4_sb_feature_ro_com(&inode->fs->sb,
				      EXT4_FRO_COM_METADATA_CSUM);
}

static int node_validate(struct ext4_inode_ref *inode, struct extent_node *node,
			 unsigned int depth)
{
	unsigned int count = node_count(node);
	unsigned int maximum = to_le16(node->header->eh_max);
	uint64_t previous = 0;

	if (to_le16(node->header->eh_magic) != EXT4_EXT_MAGIC ||
	    to_le16(node->header->eh_depth) != depth ||
	    depth > EXT4_EXT_DEPTH_MAX || !maximum ||
	    maximum > node_capacity(inode, node->root) || count > maximum ||
	    (!count && (depth || !node->root)))
		return EIO;
	for (unsigned int i = 0; i < count; i++) {
		struct ext4_extent *extent = node_extents(node) + i;
		uint64_t logical = to_le32(extent->e_blk);
		uint32_t length = depth ? 1 : extent_length(extent);
		uint64_t physical = depth ? index_block(node_indexes(node) + i)
					  : extent_block(extent);

		if ((i && logical < previous) ||
		    !block_valid(inode, physical, length) ||
		    logical + length > (uint64_t)UINT32_MAX + 1)
			return EIO;
		previous = logical + length;
	}
	if (!node->root && checksum_enabled(inode)) {
		uint32_t stored;

		memcpy(&stored, node->block.data + checksum_offset(node),
		       sizeof(stored));
		if (to_le32(stored) != node_checksum(inode, node))
			return EIO;
	}
	return 0;
}

static int node_close(struct ext4_inode_ref *inode, struct extent_node *node)
{
	return node->root ? 0 : ext4_block_set(inode->fs->bdev, &node->block);
}

static int node_read(struct ext4_inode_ref *inode, uint64_t block,
		     unsigned int depth, struct extent_node *node)
{
	int error;

	memset(node, 0, sizeof(*node));
	if (!block_valid(inode, block, 1))
		return EIO;
	error = ext4_trans_block_get(inode->fs->bdev, &node->block, block);
	if (error)
		return error;
	node->header = (struct ext4_extent_header *)node->block.data;
	error = node_validate(inode, node, depth);
	if (error)
		node_close(inode, node);
	return error;
}

static int node_dirty(struct ext4_inode_ref *inode, struct extent_node *node)
{
	if (node->root) {
		inode->dirty = true;
		return 0;
	}
	if (checksum_enabled(inode)) {
		uint32_t crc = to_le32(node_checksum(inode, node));

		memcpy(node->block.data + checksum_offset(node), &crc,
		       sizeof(crc));
	}
	return ext4_trans_set_block_dirty(node->block.buf);
}

static void node_init(struct ext4_inode_ref *inode, struct extent_node *node,
		      unsigned int depth)
{
	size_t size = node->root ? sizeof(inode->inode->blocks)
				 : ext4_sb_get_block_size(&inode->fs->sb);

	memset(node->header, 0, size);
	node->header->eh_magic = to_le16(EXT4_EXT_MAGIC);
	node->header->eh_max = to_le16(node_capacity(inode, node->root));
	node->header->eh_depth = to_le16(depth);
}

static int node_new(struct ext4_inode_ref *inode, unsigned int depth,
		    struct extent_node *node)
{
	uint64_t block;
	uint64_t goal =
	    (uint64_t)ext4_get32(&inode->fs->sb, first_data_block) + 1;
	int error = ext4_balloc_alloc_block(inode, goal, &block);

	if (error)
		return error;
	memset(node, 0, sizeof(*node));
	error =
	    ext4_trans_block_get_noread(inode->fs->bdev, &node->block, block);
	if (error) {
		ext4_balloc_free_block(inode, block);
		return error;
	}
	node->header = (struct ext4_extent_header *)node->block.data;
	node_init(inode, node, depth);
	return 0;
}

static unsigned int node_search(struct extent_node *node, uint32_t logical)
{
	struct ext4_extent *entries = node_extents(node);
	unsigned int low = 0, high = node_count(node);

	while (low < high) {
		unsigned int middle = low + (high - low) / 2;

		if (logical < to_le32(entries[middle].e_blk))
			high = middle;
		else
			low = middle + 1;
	}
	return low ? low - 1 : 0;
}

static int path_close(struct ext4_inode_ref *inode, struct extent_path *path,
		      int error)
{
	while (path->count) {
		int released = node_close(inode, &path->nodes[--path->count]);

		if (!error)
			error = released;
	}
	return error;
}

static int root_open(struct ext4_inode_ref *inode, struct extent_path *path)
{
	memset(path, 0, sizeof(*path));
	path->nodes[0].root = true;
	path->nodes[0].header = ext4_inode_get_extent_header(inode->inode);
	path->count = 1;
	return node_validate(inode, &path->nodes[0],
			     to_le16(path->nodes[0].header->eh_depth));
}

static int path_find(struct ext4_inode_ref *inode, uint32_t logical,
		     struct extent_path *path)
{
	int error = root_open(inode, path);

	if (error)
		return path_close(inode, path, error);
	while (to_le16(path->nodes[path->count - 1].header->eh_depth)) {
		struct extent_node *parent = &path->nodes[path->count - 1];
		unsigned int slot = node_search(parent, logical);
		uint64_t physical = index_block(node_indexes(parent) + slot);

		path->slots[path->count - 1] = slot;
		error = node_read(inode, physical,
				  to_le16(parent->header->eh_depth) - 1,
				  &path->nodes[path->count]);
		if (error)
			return path_close(inode, path, error);
		path->count++;
	}
	return 0;
}

static int root_grow(struct ext4_inode_ref *inode, struct extent_node *root)
{
	unsigned int depth = to_le16(root->header->eh_depth);
	struct extent_node child;
	uint64_t physical;
	uint32_t first;
	int error;

	if (depth == EXT4_EXT_DEPTH_MAX)
		return EFBIG;
	error = node_new(inode, depth, &child);
	if (error)
		return error;
	memcpy(child.header, root->header, sizeof(inode->inode->blocks));
	child.header->eh_max = to_le16(node_capacity(inode, false));
	physical = child.block.lb_id;
	first = node_first(&child);
	error = node_dirty(inode, &child);
	int released = node_close(inode, &child);
	if (error || released) {
		ext4_balloc_free_block(inode, physical);
		return error ? error : released;
	}
	node_init(inode, root, depth + 1);
	index_set(node_indexes(root), first, physical);
	node_set_count(root, 1);
	return node_dirty(inode, root);
}

static int node_split(struct ext4_inode_ref *inode, struct extent_node *parent,
		      unsigned int slot, struct extent_node *child)
{
	unsigned int count = node_count(child), left = count / 2;
	unsigned int parents = node_count(parent);
	struct extent_node right;
	struct ext4_extent_index *indexes = node_indexes(parent);
	uint64_t physical;
	uint32_t first;
	int error = node_new(inode, to_le16(child->header->eh_depth), &right);

	if (error)
		return error;
	memcpy(node_extents(&right), node_extents(child) + left,
	       (count - left) * sizeof(struct ext4_extent));
	node_set_count(&right, count - left);
	physical = right.block.lb_id;
	first = node_first(&right);
	error = node_dirty(inode, &right);
	int released = node_close(inode, &right);
	if (error || released) {
		ext4_balloc_free_block(inode, physical);
		return error ? error : released;
	}
	memmove(indexes + slot + 2, indexes + slot + 1,
		(parents - slot - 1) * sizeof(*indexes));
	index_set(indexes + slot + 1, first, physical);
	node_set_count(parent, parents + 1);
	node_set_count(child, left);
	error = node_dirty(inode, child);
	int parent_error = node_dirty(inode, parent);
	return error ? error : parent_error;
}

static int path_reserve(struct ext4_inode_ref *inode, uint32_t logical,
			unsigned int extra, struct extent_path *path)
{
	int error = path_find(inode, logical, path);

	if (error)
		return error;
	struct extent_node *leaf = &path->nodes[path->count - 1];

	if (node_count(leaf) + extra <= to_le16(leaf->header->eh_max))
		return 0;
	error = path_close(inode, path, 0);
	if (error)
		return error;
	error = root_open(inode, path);
	struct extent_node *root = &path->nodes[0];
	unsigned int needed = to_le16(root->header->eh_depth) ? 1 : extra;

	if (error)
		return path_close(inode, path, error);
	if (node_count(root) + needed > to_le16(root->header->eh_max)) {
		error = root_grow(inode, root);
		if (error)
			return path_close(inode, path, error);
	}
	while (to_le16(path->nodes[path->count - 1].header->eh_depth)) {
		struct extent_node *parent = &path->nodes[path->count - 1];
		struct extent_node *child = &path->nodes[path->count];
		unsigned int depth = to_le16(parent->header->eh_depth) - 1;
		unsigned int slot = node_search(parent, logical);

		error =
		    node_read(inode, index_block(node_indexes(parent) + slot),
			      depth, child);
		if (error)
			return path_close(inode, path, error);
		path->count++;
		needed = depth ? 1 : extra;
		if (node_count(child) + needed >
		    to_le16(child->header->eh_max)) {
			error = node_split(inode, parent, slot, child);
			if (error)
				return path_close(inode, path, error);
			if (logical >=
			    to_le32(node_indexes(parent)[slot + 1].ei_blk)) {
				error = node_close(inode, child);
				path->count--;
				if (error)
					return path_close(inode, path, error);
				slot++;
				error = node_read(
				    inode,
				    index_block(node_indexes(parent) + slot),
				    depth, child);
				if (error)
					return path_close(inode, path, error);
				path->count++;
			}
		}
		path->slots[path->count - 2] = slot;
	}
	return 0;
}

static int path_dirty(struct ext4_inode_ref *inode, struct extent_path *path)
{
	unsigned int level = path->count - 1;
	int error = node_dirty(inode, &path->nodes[level]);

	while (level && !error) {
		uint32_t first = node_first(&path->nodes[level]);
		struct extent_node *parent = &path->nodes[level - 1];
		struct ext4_extent_index *index =
		    node_indexes(parent) + path->slots[level - 1];

		if (to_le32(index->ei_blk) == first)
			break;
		index->ei_blk = to_le32(first);
		error = node_dirty(inode, parent);
		level--;
	}
	return error;
}

static void leaf_merge(struct extent_node *node)
{
	struct ext4_extent *entries = node_extents(node);
	unsigned int count = node_count(node), out = 0;

	for (unsigned int i = 0; i < count; i++) {
		if (out) {
			struct ext4_extent *left = entries + out - 1;
			uint32_t length = extent_length(left);
			uint32_t combined = length + extent_length(entries + i);
			bool unwritten = extent_unwritten(left);

			if (extent_unwritten(entries + i) == unwritten &&
			    combined <=
				EXT_INIT_MAX_LEN - (unwritten ? 1 : 0) &&
			    (uint64_t)to_le32(left->e_blk) + length ==
				to_le32(entries[i].e_blk) &&
			    extent_block(left) + length ==
				extent_block(entries + i)) {
				left->e_len =
				    to_le16(combined +
					    (unwritten ? EXT_INIT_MAX_LEN : 0));
				continue;
			}
		}
		entries[out++] = entries[i];
	}
	node_set_count(node, out);
}

static int blocks_zero(struct ext4_inode_ref *inode, uint64_t first,
		       uint32_t count)
{
	uint32_t size = ext4_sb_get_block_size(&inode->fs->sb);

	for (uint32_t i = 0; i < count; i++) {
		struct ext4_block block;
		int error = ext4_trans_block_get_noread(inode->fs->bdev, &block,
							first + i);

		if (error)
			return error;
		memset(block.data, 0, size);
		error = ext4_trans_set_block_dirty(block.buf);
		int released = ext4_block_set(inode->fs->bdev, &block);
		if (error || released)
			return error ? error : released;
	}
	return 0;
}

void ext4_extent_tree_init(struct ext4_inode_ref *inode)
{
	struct extent_node root = {
	    .root = true,
	    .header = ext4_inode_get_extent_header(inode->inode),
	};

	node_init(inode, &root, 0);
	ext4_inode_set_flag(inode->inode, EXT4_INODE_FLAG_EXTENTS);
	inode->dirty = true;
}

int ext4_extent_get_blocks(struct ext4_inode_ref *inode, ext4_lblk_t logical,
			   uint32_t maximum, ext4_fsblk_t *physical,
			   bool create, uint32_t *blocks_count)
{
	struct extent_path path;
	struct ext4_extent extent;
	struct extent_node *leaf;
	unsigned int slot, count;
	uint32_t offset = 0, length = 0;
	uint32_t ignored_count;
	bool found = false;
	int error;

	if (!maximum || !physical)
		return EINVAL;
	if (!blocks_count)
		blocks_count = &ignored_count;
	*physical = 0;
	*blocks_count = 0;
	error = path_find(inode, logical, &path);
	if (error)
		return error;
	leaf = &path.nodes[path.count - 1];
	slot = node_search(leaf, logical);
	count = node_count(leaf);
	if (count) {
		extent = node_extents(leaf)[slot];
		uint32_t start = to_le32(extent.e_blk);

		length = extent_length(&extent);
		found = logical >= start &&
			(uint64_t)logical < (uint64_t)start + length;
		offset = logical - start;
	}
	if (found && (!extent_unwritten(&extent) || !create)) {
		*blocks_count =
		    maximum < length - offset ? maximum : length - offset;
		if (!extent_unwritten(&extent))
			*physical = extent_block(&extent) + offset;
		return path_close(inode, &path, 0);
	}
	if (!create)
		return path_close(inode, &path, 0);
	error = path_close(inode, &path, 0);
	if (error)
		return error;
	uint32_t converted =
	    maximum < length - offset ? maximum : length - offset;
	unsigned int extra =
	    found ? (offset != 0) + (offset + converted < length) : 1;

	error = path_reserve(inode, logical, extra, &path);
	if (error)
		return error;
	leaf = &path.nodes[path.count - 1];
	count = node_count(leaf);
	slot = node_search(leaf, logical);
	struct ext4_extent *entries = node_extents(leaf);
	uint64_t first;
	uint32_t allocated;
	if (found) {
		extent = entries[slot];
		allocated = converted;
		first = extent_block(&extent) + offset;
		error = blocks_zero(inode, first, allocated);
		if (error)
			return path_close(inode, &path, error);
		struct ext4_extent replacements[3];
		unsigned int pieces = 0;

		if (offset)
			replacements[pieces++] =
			    extent_make(to_le32(extent.e_blk),
					extent_block(&extent), offset, true);
		replacements[pieces++] =
		    extent_make(logical, first, allocated, false);
		if (offset + allocated < length)
			replacements[pieces++] =
			    extent_make(logical + allocated, first + allocated,
					length - offset - allocated, true);
		memmove(entries + slot + pieces, entries + slot + 1,
			(count - slot - 1) * sizeof(*entries));
		memcpy(entries + slot, replacements, pieces * sizeof(*entries));
		node_set_count(leaf, count + pieces - 1);
	} else {
		uint64_t goal = count ? extent_block(entries + slot) +
					    extent_length(entries + slot)
				      : (uint64_t)ext4_get32(&inode->fs->sb,
							     first_data_block) +
					    1;

		error = ext4_balloc_alloc_block(inode, goal, &first);
		if (error)
			return path_close(inode, &path, error);
		error = blocks_zero(inode, first, 1);
		if (error) {
			ext4_balloc_free_block(inode, first);
			return path_close(inode, &path, error);
		}
		allocated = 1;
		if (count && logical > to_le32(entries[slot].e_blk))
			slot++;
		memmove(entries + slot + 1, entries + slot,
			(count - slot) * sizeof(*entries));
		entries[slot] = extent_make(logical, first, 1, false);
		node_set_count(leaf, count + 1);
	}
	leaf_merge(leaf);
	error = path_dirty(inode, &path);
	error = path_close(inode, &path, error);
	if (!error) {
		*physical = first;
		*blocks_count = allocated;
	}
	return error;
}

static int node_remove(struct ext4_inode_ref *inode, struct extent_node *node,
		       uint32_t from, uint32_t to)
{
	unsigned int depth = to_le16(node->header->eh_depth);

	for (unsigned int remaining = node_count(node); remaining;
	     remaining--) {
		unsigned int slot = remaining - 1;
		int error;

		if (depth) {
			struct ext4_extent_index *indexes = node_indexes(node);
			struct extent_node child;
			uint64_t physical = index_block(indexes + slot);
			bool empty;

			if (to_le32(indexes[slot].ei_blk) > to)
				continue;
			if (slot + 1 < node_count(node) &&
			    to_le32(indexes[slot + 1].ei_blk) <= from)
				break;
			error = node_read(inode, physical, depth - 1, &child);
			if (error)
				return error;
			error = node_remove(inode, &child, from, to);
			empty = !node_count(&child);
			if (!empty)
				indexes[slot].ei_blk =
				    to_le32(node_first(&child));
			int released = node_close(inode, &child);
			if (error || released)
				return error ? error : released;
			if (empty) {
				memmove(indexes + slot, indexes + slot + 1,
					(node_count(node) - slot - 1) *
					    sizeof(*indexes));
				node_set_count(node, node_count(node) - 1);
			}
			error = node_dirty(inode, node);
			if (error)
				return error;
			if (empty) {
				error = ext4_balloc_free_block(inode, physical);
				if (error)
					return error;
			}
		} else {
			struct ext4_extent *entries = node_extents(node);
			struct ext4_extent extent = entries[slot];
			uint64_t start = to_le32(extent.e_blk);
			uint64_t end = start + extent_length(&extent);
			uint64_t first = start > from ? start : from;
			uint64_t last =
			    end < (uint64_t)to + 1 ? end : (uint64_t)to + 1;
			uint32_t head = first - start, tail = end - last;
			uint64_t physical = extent_block(&extent);
			bool unwritten = extent_unwritten(&extent);

			if (first >= last)
				continue;
			if (head && tail) {
				memmove(entries + slot + 2, entries + slot + 1,
					(node_count(node) - slot - 1) *
					    sizeof(*entries));
				entries[slot] = extent_make(start, physical,
							    head, unwritten);
				entries[slot + 1] =
				    extent_make(last, physical + last - start,
						tail, unwritten);
				node_set_count(node, node_count(node) + 1);
			} else if (head) {
				entries[slot] = extent_make(start, physical,
							    head, unwritten);
			} else if (tail) {
				entries[slot] =
				    extent_make(last, physical + last - start,
						tail, unwritten);
			} else {
				memmove(entries + slot, entries + slot + 1,
					(node_count(node) - slot - 1) *
					    sizeof(*entries));
				node_set_count(node, node_count(node) - 1);
			}
			error = node_dirty(inode, node);
			if (error)
				return error;
			error = ext4_balloc_free_blocks(inode, physical + head,
							last - first);
			if (error)
				return error;
		}
	}
	return 0;
}

int ext4_extent_remove_space(struct ext4_inode_ref *inode, ext4_lblk_t from,
			     ext4_lblk_t to)
{
	struct extent_path path;
	bool middle = false;
	int error;

	if (from > to)
		return EINVAL;
	error = path_find(inode, from, &path);
	if (error)
		return error;
	struct extent_node *leaf = &path.nodes[path.count - 1];

	if (node_count(leaf)) {
		struct ext4_extent *extent =
		    node_extents(leaf) + node_search(leaf, from);
		uint64_t start = to_le32(extent->e_blk);

		middle = start < from &&
			 start + extent_length(extent) > (uint64_t)to + 1;
	}
	error = path_close(inode, &path, 0);
	if (error)
		return error;
	if (middle) {
		error = path_reserve(inode, from, 1, &path);
		if (error)
			return error;
		error = path_close(inode, &path, 0);
		if (error)
			return error;
	}
	error = root_open(inode, &path);
	if (!error)
		error = node_remove(inode, &path.nodes[0], from, to);
	if (!error && !node_count(&path.nodes[0]))
		ext4_extent_tree_init(inode);
	return path_close(inode, &path, error);
}
