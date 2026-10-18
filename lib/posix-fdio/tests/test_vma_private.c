/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include <uk/nofault.h>
#include <uk/posix-file-vma.h>
#include <uk/test.h>

static ssize_t private_file_read(const struct uk_file *file __unused,
				 const struct iovec *iov, size_t count,
				 size_t offset __unused, long flags __unused)
{
	size_t total = 0;

	for (size_t index = 0; index < count; index++) {
		memset(iov[index].iov_base, 0, iov[index].iov_len);
		total += iov[index].iov_len;
	}
	return total;
}

static int private_file_stat(const struct uk_file *file __unused,
			     unsigned int mask __unused, struct uk_statx *stat)
{
	stat->stx_size = 1UL << 24;
	return 0;
}

static const struct uk_file_ops private_file_ops = {
	.read = private_file_read,
	.getstat = private_file_stat,
};

struct private_file_fixture {
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_file file;
};

static struct uk_ofile *private_file_open(struct private_file_fixture *fixture)
{
	fixture->references = UK_FILE_REFCNT_INIT_VALUE(fixture->references);
	fixture->state = UK_FILE_STATE_EVENTS_INIT_VALUE(fixture->state, 0);
	fixture->file = (struct uk_file){
		.ops = &private_file_ops, .refcnt = &fixture->references,
		.state = &fixture->state
	};
	return uk_ofile_new(&fixture->file, O_RDONLY, 0);
}

static bool private_page_written(struct uk_vas *vas, __vaddr_t address)
{
	bool written;

	uk_mutex_lock(&vas->mutation_lock);
	written = uk_fdio_vma_page_written(uk_vma_find(vas, address), address);
	uk_mutex_unlock(&vas->mutation_lock);
	return written;
}

UK_TESTCASE(posix_file_vma, private_history_survives_protection_split_and_merge)
{
	struct private_file_fixture fixture;
	struct uk_ofile *description = private_file_open(&fixture);
	struct uk_fdio_vma_args args = {.of = description};
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__vaddr_t middle;
	__u32 value = 0;
	int rc;

	UK_TEST_EXPECT_NOT_NULL(description);
	if (!description)
		return;
	rc = uk_vma_map(vas, &address, 3 * UK_PAGING_PAGE_SIZE,
			UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_MAP_PRIVATE, NULL,
			&uk_fdio_vma_ops, &args, NULL);
	uk_ofile_release(description);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		return;
	middle = address + UK_PAGING_PAGE_SIZE;
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, middle,
						 UK_VMA_FAULT_READ));
	UK_TEST_EXPECT(!private_page_written(vas, middle));
	rc = uk_nofault_cmpxchg32((__u32 *)middle, &value, 9,
				  UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EFAULT);
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
					    UK_PAGING_PAGE_ATTR_PROT_READ, 0));
	UK_TEST_EXPECT(!private_page_written(vas, middle));
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, middle,
						 UK_VMA_FAULT_READ));
	rc = uk_vma_populate_page(vas, middle, UK_VMA_FAULT_WRITE);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EFAULT);
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
					    UK_PAGING_PAGE_ATTR_PROT_RW, 0));
	UK_TEST_EXPECT(!private_page_written(vas, middle));
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, middle,
						 UK_VMA_FAULT_WRITE));
	UK_TEST_EXPECT(private_page_written(vas, middle));
	UK_TEST_EXPECT(!private_page_written(vas, address));
	UK_TEST_EXPECT_ZERO(uk_nofault_cmpxchg32((__u32 *)middle, &value, 9,
						 UK_NOFAULTF_NOPAGING));
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
					    UK_PAGING_PAGE_ATTR_PROT_READ, 0));
	UK_TEST_EXPECT(private_page_written(vas, middle));
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
					    UK_PAGING_PAGE_ATTR_PROT_RW, 0));
	UK_TEST_EXPECT(uk_vma_find(vas, middle)->start == address);
	UK_TEST_EXPECT(private_page_written(vas, middle));
	UK_TEST_EXPECT_ZERO(uk_vma_advise(vas, middle, UK_PAGING_PAGE_SIZE,
					  UK_VMA_ADV_DONTNEED, 0));
	UK_TEST_EXPECT(!private_page_written(vas, middle));
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, middle,
						 UK_VMA_FAULT_READ));
	UK_TEST_EXPECT_ZERO(uk_nofault_read32((__u32 *)middle, &value,
					      UK_NOFAULTF_NOPAGING));
	UK_TEST_EXPECT_ZERO(value);
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
					 3 * UK_PAGING_PAGE_SIZE, 0));
}

#ifdef UK_PAGING_PAGE_LARGE_SHIFT
UK_TESTCASE(posix_file_vma, private_large_fault_tracks_every_base_page)
{
	struct private_file_fixture fixture;
	struct uk_ofile *description = private_file_open(&fixture);
	struct uk_fdio_vma_args args = {.of = description};
	struct uk_vas *vas = uk_vas_get_active();
	const __sz length = 1UL << UK_PAGING_PAGE_LARGE_SHIFT;
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	int rc;

	UK_TEST_EXPECT_NOT_NULL(description);
	if (!description)
		return;
	rc = uk_vma_map(vas, &address, length, UK_PAGING_PAGE_ATTR_PROT_RW,
			UK_VMA_MAP_PRIVATE |
			UK_VMA_MAP_SIZE(UK_PAGING_PAGE_LARGE_SHIFT), NULL,
			&uk_fdio_vma_ops, &args, NULL);
	uk_ofile_release(description);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		return;
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, address,
						 UK_VMA_FAULT_READ));
	UK_TEST_EXPECT(!private_page_written(vas, address));
	UK_TEST_EXPECT(!private_page_written(vas, address + length - 1));
	UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, address,
						 UK_VMA_FAULT_WRITE));
	UK_TEST_EXPECT(private_page_written(vas, address));
	UK_TEST_EXPECT(private_page_written(vas, address + length - 1));
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address, length, 0));
}
#endif

UK_TESTCASE(posix_file_vma, nofault_copy_recovers_from_file_population_error)
{
	struct private_file_fixture fixture;
	struct uk_ofile *description = private_file_open(&fixture);
	struct uk_fdio_vma_args args = {
		.of = description, .offset = 1UL << 24
	};
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	char value = 7;
	int rc;

	UK_TEST_EXPECT_NOT_NULL(description);
	if (!description)
		return;
	rc = uk_vma_map(vas, &address, UK_PAGING_PAGE_SIZE,
			UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_MAP_PRIVATE, NULL,
			&uk_fdio_vma_ops, &args, NULL);
	uk_ofile_release(description);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		return;
	UK_TEST_EXPECT_ZERO(uk_nofault_memcpy(&value, (char *)address, 1, 0));
	UK_TEST_EXPECT_SNUM_EQ(value, 7);
	UK_TEST_EXPECT_ZERO(uk_nofault_memcpy((char *)address, &value, 1, 0));
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address, UK_PAGING_PAGE_SIZE, 0));
}

uk_testsuite_register(posix_file_vma, NULL);
