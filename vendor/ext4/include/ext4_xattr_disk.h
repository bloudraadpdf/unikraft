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

#ifndef EXT4_XATTR_DISK_H_
#define EXT4_XATTR_DISK_H_

/* Linux xattr name indexes */
#define EXT4_XATTR_INDEX_USER 1
#define EXT4_XATTR_INDEX_POSIX_ACL_ACCESS 2
#define EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT 3
#define EXT4_XATTR_INDEX_TRUSTED 4
#define EXT4_XATTR_INDEX_LUSTRE 5
#define EXT4_XATTR_INDEX_SECURITY 6
#define EXT4_XATTR_INDEX_SYSTEM 7
#define EXT4_XATTR_INDEX_RICHACL 8
#define EXT4_XATTR_INDEX_ENCRYPTION 9

/* Magic value in attribute blocks */
#define EXTATTR_MAGIC 0xEA020000

/* Max EA name length */
#define EXT2_EXTATTR_NAMELEN_MAX 255

/* EA hash constants */
#define EXT2_EXTATTR_NAME_HASH_SHIFT 5
#define EXT2_EXTATTR_VALUE_HASH_SHIFT 16
#define EXT2_EXTATTR_BLOCK_HASH_SHIFT 16

struct ext2fs_extattr_header {
	uint32_t h_magic;	/* magic number for identification */
	uint32_t h_refcount;	/* reference count */
	uint32_t h_blocks;	/* number of disk blocks used */
	uint32_t h_hash;	/* hash value of all attributes */
	uint32_t h_checksum;	/* crc32c(uuid+id+xattrblock) */
				/* id = inum if refcount=1, blknum otherwise */
	uint32_t h_reserved[3]; /* zero right now */
};

struct ext2fs_extattr_dinode_header {
	uint32_t h_magic; /* magic number for identification */
};

struct ext2fs_extattr_entry {
	uint8_t e_name_len;	/* length of name */
	uint8_t e_name_index;	/* attribute name index */
	uint16_t e_value_offs;	/* offset in disk block of value */
	uint32_t e_value_block; /* disk block attribute is stored on (n/i) */
	uint32_t e_value_size;	/* size of attribute value */
	uint32_t e_hash;	/* hash value of name and value */
	char e_name[0];		/* attribute name */
};

#define EXT2_IFIRST(hdr) ((struct ext2fs_extattr_entry *)((hdr) + 1))

#define EXT2_HDR(bh) ((struct ext2fs_extattr_header *)((bh)->b_data))
#define EXT2_ENTRY(ptr) ((struct ext2fs_extattr_entry *)(ptr))
#define EXT2_IS_LAST_ENTRY(entry) (*(uint32_t *)(entry) == 0)

#define EXT2_EXTATTR_PAD_BITS 2
#define EXT2_EXTATTR_PAD (1 << EXT2_EXTATTR_PAD_BITS)
#define EXT2_EXTATTR_ROUND (EXT2_EXTATTR_PAD - 1)
#define EXT2_EXTATTR_LEN(name_len)                                             \
	(((name_len) + EXT2_EXTATTR_ROUND +                                    \
	  sizeof(struct ext2fs_extattr_entry)) &                               \
	 ~EXT2_EXTATTR_ROUND)

#define EXT2_EXTATTR_SIZE(size)                                                \
	(((size) + EXT2_EXTATTR_ROUND) & ~EXT2_EXTATTR_ROUND)

#define EXT2_EXTATTR_NEXT(entry)                                               \
	((struct ext2fs_extattr_entry *)((char *)(entry) +                     \
					 EXT2_EXTATTR_LEN(                     \
					     (entry)->e_name_len)))

#endif /* EXT4_XATTR_DISK_H_ */
