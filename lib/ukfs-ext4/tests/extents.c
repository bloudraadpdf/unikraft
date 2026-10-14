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
#include <ext4_misc.h>
#include <ext4_super.h>
#include <ext4_trans.h>
#include <ext4_xattr.h>
#include <ext4_xattr_disk.h>

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
static uint64_t interrupt_sector = UINT64_MAX;
static bool interrupt_commit, commit_written, recovered;

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

	const struct jbd_bhdr *header = buffer;
	bool is_commit = jbd_get32(header, magic) == JBD_MAGIC_NUMBER &&
			 jbd_get32(header, blocktype) == JBD_COMMIT_BLOCK;

	if ((interrupt_commit && is_commit) ||
	    (interrupt_sector >= block && interrupt_sector - block < count)) {
		REQUIRE(interrupt_commit || commit_written);
		REQUIRE(!fsync(disk_fd));
		_exit(77);
	}
	ssize_t written = pwrite(disk_fd, buffer, size, block * 512);

	if (written == (ssize_t)size && is_commit)
		commit_written = true;
	return written == (ssize_t)size ? 0 : EIO;
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
	if (ext4_sb_feature_incom(&fs.sb, EXT4_FINCOM_RECOVER)) {
		recovered = jbd_get32(&journal_fs.sb, start) != 0;
		CHECK(jbd_recover(&journal_fs));
	}
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

static uint32_t lookup_name(const char *name)
{
	struct ext4_inode_ref root;
	struct ext4_dir_search_result result;
	uint32_t number;

	CHECK(ext4_fs_get_inode_ref(&fs, EXT4_INODE_ROOT_INDEX, &root));
	CHECK(ext4_dir_find_entry(&result, &root, name, strlen(name)));
	number = ext4_dir_en_get_inode(result.dentry);
	CHECK(ext4_dir_destroy_result(&root, &result));
	CHECK(ext4_fs_put_inode_ref(&root));
	return number;
}

static uint32_t lookup(void)
{
	return lookup_name("extents");
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

static void expect_attribute(struct ext4_inode_ref *inode, const char *name,
			     unsigned char value, size_t length)
{
	unsigned char buffer[1024];
	size_t actual;

	CHECK(ext4_xattr_get(inode, 1, name, strlen(name), NULL, 0, &actual));
	REQUIRE(actual == length);
	CHECK(ext4_xattr_get(inode, 1, name, strlen(name), buffer,
			     sizeof(buffer), &actual));
	REQUIRE(actual == length);
	for (size_t i = 0; i < actual; i++)
		REQUIRE(buffer[i] == value);
	if (length)
		REQUIRE(ext4_xattr_get(inode, 1, name, strlen(name), buffer,
				       length - 1, &actual) == ERANGE);
}

static void attributes_create(void)
{
	struct ext4_inode_ref inode;
	unsigned char large[700], small[8];
	size_t block_size = ext4_sb_get_block_size(&fs.sb);
	unsigned char *oversized = calloc(1, block_size);

	REQUIRE(oversized);
	memset(large, 'A', sizeof(large));
	memset(small, 's', sizeof(small));
	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	CHECK(ext4_xattr_set(&inode, 1, "small", 5, small, sizeof(small)));
	CHECK(ext4_xattr_set(&inode, 1, "large", 5, large, sizeof(large)));
	CHECK(ext4_xattr_set(&inode, 1, "empty", 5, NULL, 0));
	REQUIRE(ext4_inode_get_file_acl(inode.inode, &fs.sb));
	REQUIRE(ext4_xattr_set(&inode, 1, "small", 5, oversized, block_size) ==
		ENOSPC);
	free(oversized);
	expect_attribute(&inode, "small", 's', sizeof(small));
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
}

static void attributes_verify(void)
{
	struct ext4_inode_ref inode;
	struct ext4_xattr_list_entry list = {0};
	size_t count;

	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	expect_attribute(&inode, "small", 's', 8);
	expect_attribute(&inode, "large", 'A', 700);
	expect_attribute(&inode, "empty", 0, 0);
	CHECK(ext4_xattr_list(&inode, &list, &count));
	REQUIRE(count == 3);
	for (struct ext4_xattr_list_entry *entry = list.next; entry;
	     entry = entry->next) {
		REQUIRE(entry->name_index == 1 && entry->name_len == 5);
		REQUIRE(!strcmp(entry->name, "small") ||
			!strcmp(entry->name, "large") ||
			!strcmp(entry->name, "empty"));
	}
	ext4_xattr_list_free(&list);
	REQUIRE(ext4_xattr_get(&inode, 1, "missing", 7, NULL, 0, &count) ==
		ENODATA);
	CHECK(ext4_fs_put_inode_ref(&inode));
}

static void attributes_clear(void)
{
	struct ext4_inode_ref inode;

	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	CHECK(ext4_xattr_remove(&inode, 1, "large", 5));
	REQUIRE(!ext4_inode_get_file_acl(inode.inode, &fs.sb));
	expect_attribute(&inode, "small", 's', 8);
	CHECK(ext4_xattr_remove(&inode, 1, "empty", 5));
	CHECK(ext4_xattr_remove(&inode, 1, "small", 5));
	REQUIRE(ext4_xattr_remove(&inode, 1, "small", 5) == ENODATA);
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
}

static void attributes_shared(void)
{
	struct ext4_inode_ref a, b, c, root;
	unsigned char value[700];

	memset(value, 'B', sizeof(value));
	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup_name("peer-a"), &a));
	CHECK(ext4_fs_get_inode_ref(&fs, lookup_name("peer-b"), &b));
	CHECK(ext4_fs_get_inode_ref(&fs, lookup_name("peer-c"), &c));
	uint64_t shared = ext4_inode_get_file_acl(a.inode, &fs.sb);

	REQUIRE(shared && shared == ext4_inode_get_file_acl(b.inode, &fs.sb));
	REQUIRE(shared == ext4_inode_get_file_acl(c.inode, &fs.sb));
	expect_attribute(&a, "large", 'A', 700);
	CHECK(ext4_xattr_set(&a, 1, "large", 5, value, sizeof(value)));
	REQUIRE(ext4_inode_get_file_acl(a.inode, &fs.sb) != shared);
	expect_attribute(&a, "large", 'B', 700);
	expect_attribute(&b, "large", 'A', 700);
	expect_attribute(&c, "large", 'A', 700);
	CHECK(ext4_fs_put_inode_ref(&a));
	CHECK(ext4_fs_get_inode_ref(&fs, EXT4_INODE_ROOT_INDEX, &root));
	CHECK(ext4_dir_remove_entry(&root, "peer-b", 6));
	CHECK(ext4_fs_free_inode(&b));
	ext4_inode_set_links_cnt(b.inode, 0);
	ext4_inode_set_del_time(b.inode, UINT32_MAX);
	CHECK(ext4_fs_put_inode_ref(&b));
	expect_attribute(&c, "large", 'A', 700);
	CHECK(ext4_dir_remove_entry(&root, "peer-c", 6));
	CHECK(ext4_fs_free_inode(&c));
	ext4_inode_set_links_cnt(c.inode, 0);
	ext4_inode_set_del_time(c.inode, UINT32_MAX);
	CHECK(ext4_fs_put_inode_ref(&c));
	CHECK(ext4_fs_put_inode_ref(&root));
	commit();
}

static void interrupted_write(bool before_commit)
{
	struct ext4_inode_ref inode;
	struct ext4_block block;
	unsigned char value[700];
	uint64_t physical;
	uint32_t logical = 4096, count;

	memset(value, 'R', sizeof(value));
	begin();
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	interrupt_commit = before_commit;
	interrupt_sector =
	    before_commit
		? UINT64_MAX
		: inode.block.lb_id * (ext4_sb_get_block_size(&fs.sb) / 512);
	commit_written = false;
	CHECK(ext4_xattr_set(&inode, 1, "recovery", 8, value, sizeof(value)));
	CHECK(ext4_extent_get_blocks(&inode, logical, 1, &physical, true,
				     &count));
	REQUIRE(physical && count == 1);
	CHECK(ext4_trans_block_get(&device, &block, physical));
	memcpy(block.data, &logical, sizeof(logical));
	CHECK(ext4_trans_set_block_dirty(block.buf));
	CHECK(ext4_block_set(&device, &block));
	ext4_inode_set_size(inode.inode, (uint64_t)(logical + 1) *
					     ext4_sb_get_block_size(&fs.sb));
	inode.dirty = true;
	CHECK(ext4_fs_put_inode_ref(&inode));
	commit();
	CHECK(ext4_block_cache_flush(&device));
	CHECK(jbd_journal_stop(&journal));
	REQUIRE(0);
}

static void verify_recovery(bool committed)
{
	struct ext4_inode_ref inode;
	size_t length;

	REQUIRE(recovered);
	CHECK(ext4_fs_get_inode_ref(&fs, lookup(), &inode));
	verify_block(&inode, 4096, committed);
	if (committed)
		expect_attribute(&inode, "recovery", 'R', 700);
	else
		REQUIRE(ext4_xattr_get(&inode, 1, "recovery", 8, NULL, 0,
				       &length) == ENODATA);
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
	} else if (!strcmp(argv[2], "attributes-create")) {
		attributes_create();
		attributes_verify();
	} else if (!strcmp(argv[2], "attributes-clear")) {
		attributes_verify();
		attributes_clear();
	} else if (!strcmp(argv[2], "attributes-shared")) {
		attributes_shared();
	} else if (!strcmp(argv[2], "interrupt-commit")) {
		interrupted_write(false);
	} else if (!strcmp(argv[2], "interrupt-abort")) {
		interrupted_write(true);
	} else if (!strcmp(argv[2], "recover-commit")) {
		verify_recovery(true);
	} else if (!strcmp(argv[2], "recover-abort")) {
		verify_recovery(false);
	} else {
		REQUIRE(0);
	}
	close_image();
	puts("EXTENTS_OK");
	return 0;
}
