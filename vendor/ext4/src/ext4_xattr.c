/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2017, Fedor Uporov
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
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <ext4_balloc.h>
#include <ext4_crc32.h>
#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <ext4_xattr.h>
#include <ext4_xattr_disk.h>

struct attributes {
	struct ext4_xattr_info *items;
	size_t count;
	struct ext4_block block;
	uint8_t *inline_header;
	size_t inline_size;
};

static const struct {
	uint8_t index;
	const char *prefix;
	bool exact;
} namespaces[] = {
    {1, "user.", false},
    {2, "system.posix_acl_access", true},
    {3, "system.posix_acl_default", true},
    {4, "trusted.", false},
    {6, "security.", false},
    {8, "system.richacl", true},
    {7, "system.", false},
    {9, "encryption.", false},
};

const char *ext4_get_xattr_name_prefix(uint8_t index, size_t *length)
{
	for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]);
	     i++) {
		if (namespaces[i].index == index) {
			*length = strlen(namespaces[i].prefix);
			return namespaces[i].prefix;
		}
	}
	*length = 0;
	return NULL;
}

const char *ext4_extract_xattr_name(const char *name, size_t length,
				    uint8_t *index, size_t *name_length,
				    bool *found)
{
	*found = false;
	*index = 0;
	*name_length = 0;
	for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]);
	     i++) {
		size_t prefix = strlen(namespaces[i].prefix);

		if (length < prefix ||
		    (namespaces[i].exact && length != prefix) ||
		    (!namespaces[i].exact && length == prefix) ||
		    memcmp(name, namespaces[i].prefix, prefix))
			continue;
		*found = true;
		*index = namespaces[i].index;
		*name_length = length - prefix;
		return name + prefix;
	}
	return name;
}

static size_t aligned(size_t size)
{
	return (size + EXT2_EXTATTR_ROUND) & ~(size_t)EXT2_EXTATTR_ROUND;
}

static int compare(const void *left, const void *right)
{
	const struct ext4_xattr_info *a = left, *b = right;

	if (a->name_index != b->name_index)
		return (int)a->name_index - b->name_index;
	if (a->name_len != b->name_len)
		return a->name_len < b->name_len ? -1 : 1;
	return a->name_len ? memcmp(a->name, b->name, a->name_len) : 0;
}

static int append(struct attributes *attrs, struct ext4_xattr_info item)
{
	void *items = realloc(attrs->items, (attrs->count + 1) * sizeof(item));

	if (!items)
		return ENOMEM;
	attrs->items = items;
	attrs->items[attrs->count++] = item;
	return 0;
}

static uint32_t block_checksum(struct ext4_inode_ref *inode, uint64_t number,
			       const uint8_t *data)
{
	size_t offset = offsetof(struct ext2fs_extattr_header, h_checksum);
	uint64_t disk_number = to_le64(number);
	uint32_t zero = 0;
	uint32_t crc = ext4_sb_csum_seed(&inode->fs->sb);

	crc = ext4_crc32c(crc, &disk_number, sizeof(disk_number));
	crc = ext4_crc32c(crc, data, offset);
	crc = ext4_crc32c(crc, &zero, sizeof(zero));
	offset += sizeof(zero);
	return ext4_crc32c(crc, data + offset,
			   ext4_sb_get_block_size(&inode->fs->sb) - offset);
}

static int block_dirty(struct ext4_inode_ref *inode, struct ext4_block *block)
{
	struct ext2fs_extattr_header *header = (void *)block->data;

	if (ext4_sb_feature_ro_com(&inode->fs->sb, EXT4_FRO_COM_METADATA_CSUM))
		header->h_checksum =
		    to_le32(block_checksum(inode, block->lb_id, block->data));
	return ext4_trans_set_block_dirty(block->buf);
}

static int parse(struct attributes *attrs, uint8_t *data, size_t size,
		 size_t first)
{
	size_t offset = first, minimum = size;

	while (offset <= size && size - offset >= sizeof(uint32_t)) {
		uint32_t marker;
		struct ext2fs_extattr_entry *entry = (void *)(data + offset);

		memcpy(&marker, entry, sizeof(marker));
		if (!marker)
			return offset + sizeof(marker) <= minimum ? 0 : EIO;
		if (size - offset < sizeof(*entry))
			return EIO;
		size_t next = offset + EXT2_EXTATTR_LEN(entry->e_name_len);
		size_t value_offset = to_le16(entry->e_value_offs);
		size_t value_size = to_le32(entry->e_value_size);

		if (entry->e_value_block)
			return ENOTSUP;
		if (!entry->e_name_index || next > size ||
		    value_offset > size || value_size > size - value_offset ||
		    (value_size && (value_offset & EXT2_EXTATTR_ROUND)))
			return EIO;
		if (value_size && value_offset < minimum)
			minimum = value_offset;
		struct ext4_xattr_info item = {
		    .name_index = entry->e_name_index,
		    .name = entry->e_name,
		    .name_len = entry->e_name_len,
		    .value = data + value_offset,
		    .value_len = value_size,
		};
		int error = append(attrs, item);

		if (error)
			return error;
		offset = next;
	}
	return EIO;
}

static int attributes_close(struct ext4_inode_ref *inode,
			    struct attributes *attrs, int error)
{
	free(attrs->items);
	if (attrs->block.buf) {
		int released = ext4_block_set(inode->fs->bdev, &attrs->block);

		if (!error)
			error = released;
	}
	return error;
}

static int attributes_open(struct ext4_inode_ref *inode,
			   struct attributes *attrs)
{
	struct ext4_sblock *sb = &inode->fs->sb;
	size_t size = ext4_get16(sb, inode_size);
	size_t extra = ext4_inode_get_extra_isize(sb, inode->inode);
	uint64_t physical = ext4_inode_get_file_acl(inode->inode, sb);
	int error;

	memset(attrs, 0, sizeof(*attrs));
	if (size < EXT4_GOOD_OLD_INODE_SIZE ||
	    extra > size - EXT4_GOOD_OLD_INODE_SIZE ||
	    (extra & EXT2_EXTATTR_ROUND))
		return EIO;
	size_t offset = EXT4_GOOD_OLD_INODE_SIZE + extra;

	if (extra && size - offset >= 8) {
		uint32_t magic;

		attrs->inline_header = (uint8_t *)inode->inode + offset;
		attrs->inline_size = size - offset;
		memcpy(&magic, attrs->inline_header, sizeof(magic));
		if (magic && to_le32(magic) != EXTATTR_MAGIC)
			return EIO;
		if (magic) {
			error = parse(attrs, attrs->inline_header + 4,
				      attrs->inline_size - 4, 0);
			if (error)
				return error;
		}
	}
	if (physical) {
		uint32_t block_size = ext4_sb_get_block_size(sb);

		if (physical <= ext4_get32(sb, first_data_block) ||
		    physical >= ext4_sb_get_blocks_cnt(sb) ||
		    ext4_inode_get_blocks_count(sb, inode->inode) <
			block_size / 512)
			return EIO;
		error = ext4_trans_block_get(inode->fs->bdev, &attrs->block,
					     physical);
		if (error)
			return error;
		struct ext2fs_extattr_header *header =
		    (void *)attrs->block.data;

		if (to_le32(header->h_magic) != EXTATTR_MAGIC ||
		    to_le32(header->h_blocks) != 1 ||
		    !to_le32(header->h_refcount))
			return EIO;
		if (ext4_sb_feature_ro_com(sb, EXT4_FRO_COM_METADATA_CSUM) &&
		    to_le32(header->h_checksum) !=
			block_checksum(inode, physical, attrs->block.data))
			return EIO;
		error = parse(attrs, attrs->block.data, block_size,
			      sizeof(*header));
		if (error)
			return error;
	}
	if (attrs->count > 1) {
		qsort(attrs->items, attrs->count, sizeof(*attrs->items),
		      compare);
		for (size_t i = 1; i < attrs->count; i++)
			if (!compare(&attrs->items[i - 1], &attrs->items[i]))
				return EIO;
	}
	return 0;
}

static int validate_name(uint8_t index, const char *name, size_t length)
{
	if (!index || (length && !name) || length > EXT2_EXTATTR_NAMELEN_MAX ||
	    (length && memchr(name, 0, length)))
		return EINVAL;
	return 0;
}

static size_t find(struct attributes *attrs, const struct ext4_xattr_info *item)
{
	for (size_t i = 0; i < attrs->count; i++)
		if (!compare(&attrs->items[i], item))
			return i;
	return attrs->count;
}

int ext4_xattr_get(struct ext4_inode_ref *inode, uint8_t index,
		   const char *name, size_t length, void *buffer,
		   size_t capacity, size_t *data_length)
{
	struct attributes attrs;
	struct ext4_xattr_info key = {
	    .name_index = index, .name = name, .name_len = length};
	int error = validate_name(index, name, length);

	if (error || !data_length)
		return EINVAL;
	*data_length = 0;
	error = attributes_open(inode, &attrs);
	if (!error) {
		size_t i = find(&attrs, &key);

		if (i == attrs.count) {
			error = ENODATA;
		} else {
			*data_length = attrs.items[i].value_len;
			if (buffer && capacity < *data_length)
				error = ERANGE;
			else if (buffer && *data_length)
				memcpy(buffer, attrs.items[i].value,
				       *data_length);
		}
	}
	return attributes_close(inode, &attrs, error);
}

void ext4_xattr_list_free(struct ext4_xattr_list_entry *list)
{
	while (list->next) {
		struct ext4_xattr_list_entry *entry = list->next;

		list->next = entry->next;
		free(entry);
	}
}

int ext4_xattr_list(struct ext4_inode_ref *inode,
		    struct ext4_xattr_list_entry *list, size_t *length)
{
	struct attributes attrs;
	struct ext4_xattr_list_entry *tail = list;

	if (!list || !length)
		return EINVAL;
	list->next = NULL;
	*length = 0;
	int error = attributes_open(inode, &attrs);

	for (size_t i = 0; !error && i < attrs.count; i++) {
		struct ext4_xattr_info *item = &attrs.items[i];
		struct ext4_xattr_list_entry *entry =
		    calloc(1, sizeof(*entry) + item->name_len + 1);

		if (!entry) {
			error = ENOMEM;
			break;
		}
		entry->name_index = item->name_index;
		entry->name_len = item->name_len;
		entry->name = (char *)(entry + 1);
		memcpy(entry->name, item->name, item->name_len);
		tail->next = entry;
		tail = entry;
		(*length)++;
	}
	error = attributes_close(inode, &attrs, error);
	if (error) {
		ext4_xattr_list_free(list);
		*length = 0;
	}
	return error;
}

static uint32_t entry_hash(const struct ext2fs_extattr_entry *entry,
			   const uint8_t *value)
{
	uint32_t hash = 0;

	for (size_t i = 0; i < entry->e_name_len; i++)
		hash = (hash << EXT2_EXTATTR_NAME_HASH_SHIFT) ^
		       (hash >> (32 - EXT2_EXTATTR_NAME_HASH_SHIFT)) ^
		       (uint8_t)entry->e_name[i];
	for (size_t i = 0; i < aligned(to_le32(entry->e_value_size)); i += 4) {
		uint32_t word;

		memcpy(&word, value + i, sizeof(word));
		hash = (hash << EXT2_EXTATTR_VALUE_HASH_SHIFT) ^
		       (hash >> (32 - EXT2_EXTATTR_VALUE_HASH_SHIFT)) ^
		       to_le32(word);
	}
	return hash;
}

static bool pack(uint8_t *data, size_t *entries, size_t *values,
		 const struct ext4_xattr_info *item, bool external)
{
	size_t entry_size = EXT2_EXTATTR_LEN(item->name_len);
	size_t value_size = aligned(item->value_len);

	if (value_size > *values ||
	    *values - value_size < *entries + entry_size + 4)
		return false;
	struct ext2fs_extattr_entry *entry = (void *)(data + *entries);

	*values -= value_size;
	entry->e_name_index = item->name_index;
	entry->e_name_len = item->name_len;
	entry->e_value_size = to_le32(item->value_len);
	entry->e_value_offs = item->value_len ? to_le16(*values) : 0;
	if (item->name_len)
		memcpy(entry->e_name, item->name, item->name_len);
	if (item->value_len)
		memcpy(data + *values, item->value, item->value_len);
	if (external)
		entry->e_hash = to_le32(entry_hash(entry, data + *values));
	*entries += entry_size;
	return true;
}

static int release_block(struct ext4_inode_ref *inode, struct ext4_block *block)
{
	struct ext4_sblock *sb = &inode->fs->sb;
	struct ext2fs_extattr_header *header = (void *)block->data;
	uint32_t references = to_le32(header->h_refcount);
	uint64_t physical = block->lb_id;
	int error = 0;

	if (references > 1) {
		header->h_refcount = to_le32(references - 1);
		error = block_dirty(inode, block);
	}
	int released = ext4_block_set(inode->fs->bdev, block);
	block->buf = NULL;
	if (error || released)
		return error ? error : released;
	if (references == 1)
		return ext4_balloc_free_block(inode, physical);
	uint64_t blocks = ext4_inode_get_blocks_count(sb, inode->inode);

	error = ext4_inode_set_blocks_count(
	    sb, inode->inode, blocks - ext4_sb_get_block_size(sb) / 512);
	if (!error)
		inode->dirty = true;
	return error;
}

static int store(struct ext4_inode_ref *inode, struct attributes *attrs)
{
	struct ext4_sblock *sb = &inode->fs->sb;
	size_t size = ext4_sb_get_block_size(sb);
	uint8_t *disk = calloc(1, size + attrs->inline_size);

	if (!disk)
		return ENOMEM;
	uint8_t *inlined = disk + size;
	size_t inline_entries = 0,
	       inline_values = attrs->inline_size ? attrs->inline_size - 4 : 0;
	size_t entries = sizeof(struct ext2fs_extattr_header), values = size;
	struct ext2fs_extattr_header *header = (void *)disk;
	int error = 0;

	if (attrs->count > 1)
		qsort(attrs->items, attrs->count, sizeof(*attrs->items),
		      compare);
	for (size_t i = 0; i < attrs->count; i++) {
		if (attrs->inline_size &&
		    pack(inlined + 4, &inline_entries, &inline_values,
			 &attrs->items[i], false))
			continue;
		if (!pack(disk, &entries, &values, &attrs->items[i], true)) {
			error = ENOSPC;
			goto out;
		}
	}
	if (inline_entries) {
		uint32_t magic = to_le32(EXTATTR_MAGIC);

		memcpy(inlined, &magic, sizeof(magic));
	}
	uint64_t physical = 0;
	bool external = entries > sizeof(*header);
	bool reuse = external && attrs->block.buf &&
		     to_le32(((struct ext2fs_extattr_header *)attrs->block.data)
				 ->h_refcount) == 1;

	if (external) {
		header->h_magic = to_le32(EXTATTR_MAGIC);
		header->h_refcount = to_le32(1);
		header->h_blocks = to_le32(1);
		uint32_t hash = 0;
		struct ext2fs_extattr_entry *entry = (void *)(header + 1);

		while ((uint8_t *)entry < disk + entries) {
			uint32_t entry_value = to_le32(entry->e_hash);

			if (!entry_value) {
				hash = 0;
				break;
			}
			hash = (hash << EXT2_EXTATTR_BLOCK_HASH_SHIFT) ^
			       (hash >> (32 - EXT2_EXTATTR_BLOCK_HASH_SHIFT)) ^
			       entry_value;
			entry = EXT2_EXTATTR_NEXT(entry);
		}
		header->h_hash = to_le32(hash);
		struct ext4_block block;

		if (reuse) {
			block = attrs->block;
			physical = block.lb_id;
		} else {
			error = ext4_balloc_alloc_block(
			    inode,
			    (uint64_t)ext4_get32(sb, first_data_block) + 1,
			    &physical);
			if (error)
				goto out;
			error = ext4_trans_block_get_noread(inode->fs->bdev,
							    &block, physical);
			if (error) {
				ext4_balloc_free_block(inode, physical);
				goto out;
			}
		}
		memcpy(block.data, disk, size);
		error = block_dirty(inode, &block);
		if (!reuse) {
			int released = ext4_block_set(inode->fs->bdev, &block);

			if (!error)
				error = released;
			if (error)
				ext4_balloc_free_block(inode, physical);
		}
		if (error)
			goto out;
	}
	if (attrs->block.buf && !reuse) {
		error = release_block(inode, &attrs->block);
		if (error) {
			if (physical)
				ext4_balloc_free_block(inode, physical);
			goto out;
		}
	}
	if (attrs->inline_size)
		memcpy(attrs->inline_header, inlined, attrs->inline_size);
	ext4_inode_set_file_acl(inode->inode, sb, physical);
	inode->dirty = true;
out:
	free(disk);
	return error;
}

static int update(struct ext4_inode_ref *inode, struct ext4_xattr_info item,
		  bool remove)
{
	struct attributes attrs;
	int error = validate_name(item.name_index, item.name, item.name_len);

	if (error || (item.value_len && !item.value))
		return EINVAL;
	if (inode->fs->read_only)
		return EROFS;
	if (item.value_len > ext4_sb_get_block_size(&inode->fs->sb))
		return ENOSPC;
	if (!ext4_sb_feature_com(&inode->fs->sb, EXT4_FCOM_EXT_ATTR))
		return ENOTSUP;
	error = attributes_open(inode, &attrs);
	if (error)
		return attributes_close(inode, &attrs, error);
	size_t i = find(&attrs, &item);

	if (remove) {
		if (i == attrs.count)
			error = ENODATA;
		else {
			memmove(attrs.items + i, attrs.items + i + 1,
				(attrs.count - i - 1) * sizeof(item));
			attrs.count--;
		}
	} else if (i == attrs.count) {
		error = append(&attrs, item);
	} else {
		attrs.items[i] = item;
	}
	if (!error)
		error = store(inode, &attrs);
	return attributes_close(inode, &attrs, error);
}

int ext4_xattr_set(struct ext4_inode_ref *inode, uint8_t index,
		   const char *name, size_t length, const void *value,
		   size_t value_length)
{
	struct ext4_xattr_info item = {index, name, length, value,
				       value_length};

	return update(inode, item, false);
}

int ext4_xattr_remove(struct ext4_inode_ref *inode, uint8_t index,
		      const char *name, size_t length)
{
	struct ext4_xattr_info item = {
	    .name_index = index, .name = name, .name_len = length};

	return update(inode, item, true);
}

int ext4_xattr_free(struct ext4_inode_ref *inode)
{
	struct attributes attrs;

	if (inode->fs->read_only)
		return EROFS;
	int error = attributes_open(inode, &attrs);

	if (!error && attrs.block.buf)
		error = release_block(inode, &attrs.block);
	if (!error) {
		ext4_inode_set_file_acl(inode->inode, &inode->fs->sb, 0);
		if (attrs.inline_size)
			memset(attrs.inline_header, 0, attrs.inline_size);
		inode->dirty = true;
	}
	return attributes_close(inode, &attrs, error);
}
