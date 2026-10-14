/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ext4_blockdev.h>
#include <ext4_dir.h>
#include <ext4_extent.h>
#include <ext4_extent_disk.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_journal.h>
#include <ext4_super.h>
#include <ext4_trans.h>

#define CHECK(call)                                                            \
	do {                                                                   \
		int error = (call);                                            \
		if (error) {                                                   \
			fprintf(stderr, "%s:%d: %s: %d\n", __FILE__, __LINE__, \
				#call, error);                                 \
			exit(1);                                               \
		}                                                              \
	} while (0)
#define REQUIRE(test)                                                          \
	do {                                                                   \
		if (!(test)) {                                                 \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__,     \
				#test);                                        \
			exit(1);                                               \
		}                                                              \
	} while (0)

static int disk_fd;
static struct ext4_fs fs;
static struct ext4_bcache cache;
static struct jbd_fs journal_fs;
static struct jbd_journal journal;
static uint8_t sector[512];

static int disk_open(struct ext4_blockdev *device)
{
	(void)device;
	return 0;
}

static int disk_read(struct ext4_blockdev *device, void *buffer, uint64_t block,
		     uint32_t count)
{
	(void)device;
	size_t size = (size_t)count * 512;

	return pread(disk_fd, buffer, size, block * 512) == (ssize_t)size ? 0
									  : EIO;
}

static int disk_write(struct ext4_blockdev *device, const void *buffer,
		      uint64_t block, uint32_t count)
{
	(void)device;
	size_t size = (size_t)count * 512;

	return pwrite(disk_fd, buffer, size, block * 512) == (ssize_t)size
		   ? 0
		   : EIO;
}

static struct ext4_blockdev_iface interface = {
    .open = disk_open,
    .close = disk_open,
    .bread = disk_read,
    .bwrite = disk_write,
    .ph_bsize = 512,
    .ph_bbuf = sector,
};
static struct ext4_blockdev device = {.bdif = &interface};

static void begin(void)
{
	fs.curr_trans = jbd_journal_new_trans(&journal);
	REQUIRE(fs.curr_trans);
}

static void commit(void)
{
	CHECK(jbd_journal_commit_trans(&journal, fs.curr_trans));
	fs.curr_trans = NULL;
}

static void open_image(const char *path)
{
	struct stat stat;

	disk_fd = open(path, O_RDWR);
	REQUIRE(disk_fd >= 0 && !fstat(disk_fd, &stat));
	interface.ph_bcnt = stat.st_size / 512;
	device.part_size = stat.st_size;
	CHECK(ext4_block_init(&device));
	CHECK(ext4_fs_init(&fs, &device, false));
	REQUIRE(!fs.read_only);
	uint32_t size = ext4_sb_get_block_size(&fs.sb);

	ext4_block_set_lb_size(&device, size);
	CHECK(ext4_bcache_init_dynamic(&cache, 64, size));
	CHECK(ext4_block_bind_bcache(&device, &cache));
	device.fs = &fs;
	CHECK(jbd_get_fs(&fs, &journal_fs));
	if (ext4_sb_feature_incom(&fs.sb, EXT4_FINCOM_RECOVER))
		CHECK(jbd_recover(&journal_fs));
	CHECK(jbd_journal_start(&journal_fs, &journal));
	fs.jbd_fs = &journal_fs;
	fs.jbd_journal = &journal;
}

static void close_image(void)
{
	CHECK(jbd_journal_stop(&journal));
	CHECK(jbd_put_fs(&journal_fs));
	fs.jbd_journal = NULL;
	fs.jbd_fs = NULL;
	CHECK(ext4_block_cache_flush(&device));
	CHECK(ext4_fs_fini(&fs));
	ext4_bcache_cleanup(&cache);
	CHECK(ext4_block_fini(&device));
	REQUIRE(!fsync(disk_fd) && !close(disk_fd));
}

static uint32_t lookup(void)
{
	struct ext4_inode_ref root;
	struct ext4_dir_search_result result;
	uint32_t number;

	CHECK(ext4_fs_get_inode_ref(&fs, EXT4_INODE_ROOT_INDEX, &root));
	CHECK(ext4_dir_find_entry(&result, &root, "extents", 7));
	number = ext4_dir_en_get_inode(result.dentry);
	CHECK(ext4_dir_destroy_result(&root, &result));
	CHECK(ext4_fs_put_inode_ref(&root));
	return number;
}

static void write_block(uint32_t number, uint32_t logical, uint32_t limit)
{
	struct ext4_inode_ref inode;
	struct ext4_block block;
	uint32_t count, size = ext4_sb_get_block_size(&fs.sb);
	uint64_t physical;

	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	CHECK(ext4_extent_get_blocks(&inode, logical, 1, &physical, true,
				     &count));
	REQUIRE(physical && count == 1);
	CHECK(ext4_trans_block_get(&device, &block, physical));
	for (uint32_t offset = 0; offset < size; offset++)
		REQUIRE(block.data[offset] == 0);
	memcpy(block.data, &logical, sizeof(logical));
	CHECK(ext4_trans_set_block_dirty(block.buf));
	CHECK(ext4_block_set(&device, &block));
	ext4_inode_set_size(inode.inode, (uint64_t)limit * size);
	inode.dirty = true;
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
}

static void verify_block(struct ext4_inode_ref *inode, uint32_t logical,
			 bool allocated)
{
	uint32_t count;
	uint64_t physical;

	CHECK(ext4_extent_get_blocks(inode, logical, 1, &physical, false,
				     &count));
	if (!allocated) {
		REQUIRE(!physical);
		return;
	}
	struct ext4_block block;
	uint32_t actual;

	REQUIRE(physical && count == 1);
	CHECK(ext4_block_get(&device, &block, physical));
	memcpy(&actual, block.data, sizeof(actual));
	REQUIRE(actual == logical);
	CHECK(ext4_block_set(&device, &block));
}

static void create_file(void)
{
	struct ext4_inode_ref root, inode;
	uint32_t number;

	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, EXT4_INODE_ROOT_INDEX, &root));
	CHECK(ext4_fs_alloc_inode(&fs, &inode, EXT4_DE_REG_FILE));
	number = inode.index;
	ext4_inode_set_mode(&fs.sb, inode.inode, EXT4_INODE_MODE_FILE | 0600);
	ext4_inode_set_links_cnt(inode.inode, 1);
	ext4_extent_tree_init(&inode);
	CHECK(ext4_dir_add_entry(&root, "extents", 7, &inode));
	CHECK(ext4_fs_put_inode_ref(&root));
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
	for (uint32_t i = 0; i < 1500; i++) {
		uint32_t logical = ((i * 1009) % 1500) * 2;

		write_block(number, logical, 3000);
	}
	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	REQUIRE(to_le16(ext4_inode_get_extent_header(inode.inode)->eh_depth) >=
		2);
	CHECK(ext4_fs_put_inode_ref(&inode));
}

static void verify_file(uint32_t limit)
{
	struct ext4_inode_ref inode;
	uint32_t size = ext4_sb_get_block_size(&fs.sb);

	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	REQUIRE(ext4_inode_get_size(&fs.sb, inode.inode) ==
		(uint64_t)limit * size);
	for (uint32_t logical = 0; logical < 3000; logical++)
		verify_block(&inode, logical,
			     logical < limit && !(logical % 2));
	CHECK(ext4_fs_put_inode_ref(&inode));
}

static void truncate_file(uint32_t limit)
{
	struct ext4_inode_ref inode;

	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	CHECK(ext4_fs_truncate_inode(
	    &inode, (uint64_t)limit * ext4_sb_get_block_size(&fs.sb)));
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
}

static void remove_range(uint32_t first, uint32_t last)
{
	struct ext4_inode_ref inode;

	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	CHECK(ext4_extent_remove_space(&inode, first, last));
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
}

static void contiguous_ranges(void)
{
	uint32_t number = lookup();
	struct ext4_inode_ref inode;

	for (uint32_t logical = 0; logical < 128; logical++)
		write_block(number, logical, 128);
	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	struct ext4_extent_header *header =
	    ext4_inode_get_extent_header(inode.inode);
	REQUIRE(to_le16(header->eh_depth) == 0 &&
		to_le16(header->eh_ecount) == 1);
	CHECK(ext4_fs_put_inode_ref(&inode));
	remove_range(31, 63);
	remove_range(0, 7);
	remove_range(120, UINT32_MAX);
	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	for (uint32_t logical = 0; logical < 128; logical++)
		verify_block(&inode, logical,
			     (logical >= 8 && logical < 31) ||
				 (logical >= 64 && logical < 120));
	CHECK(ext4_fs_put_inode_ref(&inode));
}

static void unwritten_ranges(void)
{
	uint32_t number = lookup();
	struct ext4_inode_ref inode;

	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	for (uint32_t logical = 0; logical < 128; logical++)
		verify_block(&inode, logical, false);
	CHECK(ext4_fs_put_inode_ref(&inode));
	write_block(number, 63, 128);
	write_block(number, 0, 128);
	write_block(number, 127, 128);
	CHECK(ext4_fs_get_inode_ref(&fs, number, &inode));
	for (uint32_t logical = 0; logical < 128; logical++)
		verify_block(&inode, logical,
			     logical == 0 || logical == 63 || logical == 127);
	CHECK(ext4_fs_put_inode_ref(&inode));
}

int main(int argc, char **argv)
{
	REQUIRE(argc == 3);
	open_image(argv[1]);
	if (!strcmp(argv[2], "create")) {
		create_file();
		verify_file(3000);
	} else if (!strcmp(argv[2], "truncate")) {
		verify_file(3000);
		truncate_file(777);
		verify_file(777);
	} else if (!strcmp(argv[2], "clear")) {
		verify_file(777);
		truncate_file(0);
		verify_file(0);
	} else if (!strcmp(argv[2], "ranges")) {
		contiguous_ranges();
	} else if (!strcmp(argv[2], "reset")) {
		truncate_file(0);
		verify_file(0);
	} else if (!strcmp(argv[2], "unwritten")) {
		unwritten_ranges();
	} else {
		REQUIRE(0);
	}
	close_image();
	puts("EXTENTS_OK");
	return 0;
}
