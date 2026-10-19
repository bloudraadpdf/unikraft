/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/nofault.h>
#include <uk/posix-file-vma.h>
#include <uk/test.h>

static unsigned char shared_buffers[3][UK_PAGING_PAGE_SIZE]
	__align(UK_PAGING_PAGE_SIZE);

struct shared_file_fixture {
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_file file;
	unsigned int held[3];
	unsigned int acquired, released;
	int invalid_release;
};

static int shared_file_stat(const struct uk_file *file __unused,
			    unsigned int mask __unused, struct uk_statx *stat)
{
	stat->stx_size = sizeof(shared_buffers);
	return 0;
}

static ssize_t shared_file_mem(const struct uk_file *file,
			      enum uk_file_mem_op operation, size_t offset,
			      size_t length, struct iovec *iov, size_t count)
{
	struct shared_file_fixture *fixture =
		__containerof(file, struct shared_file_fixture, file);
	size_t index = offset / UK_PAGING_PAGE_SIZE;

	if (!length)
		return 0;
	if (index >= 3 || length != UK_PAGING_PAGE_SIZE ||
	    !UK_PAGING_PAGE_ALIGNED(offset))
		return -EINVAL;
	switch (operation) {
	case UKFILE_MEM_ACQUIRE:
		fixture->held[index]++;
		fixture->acquired++;
		return length;
	case UKFILE_MEM_RELEASE:
		if (!fixture->held[index])
			fixture->invalid_release = 1;
		else
			fixture->held[index]--;
		fixture->released++;
		return 0;
	case UKFILE_MEM_RETRIEVE:
		if (!fixture->held[index] || !count)
			return -ENOENT;
		iov[0] = (struct iovec){shared_buffers[index], length};
		return 1;
	default:
		return -ENODEV;
	}
}

static const struct uk_file_ops shared_file_ops = {
	.getstat = shared_file_stat, .mem = shared_file_mem
};

UK_TESTCASE(posix_file_vma, shared_history_survives_split_merge_and_refault)
{
	struct shared_file_fixture fixture = {0};
	struct uk_vas *vas = uk_vas_get_active();
	struct uk_ofile *description;
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	const unsigned int order[] = {2, 0, 1};
	int rc;

	fixture.references = UK_FILE_REFCNT_INIT_VALUE(fixture.references);
	fixture.state = UK_FILE_STATE_EVENTS_INIT_VALUE(fixture.state, 0);
	fixture.file = (struct uk_file){
		.ops = &shared_file_ops, .refcnt = &fixture.references,
		.state = &fixture.state
	};
	description = uk_ofile_new(&fixture.file, O_RDWR, 0);
	UK_TEST_EXPECT_NOT_NULL(description);
	if (!description)
		return;
	struct uk_fdio_vma_args args = {.of = description};

	rc = uk_vma_map(vas, &address, sizeof(shared_buffers),
			UK_PAGING_PAGE_ATTR_PROT_RW,
			UK_VMA_FILE_SHARED | UK_VMA_MAP_SIZE(UK_PAGING_PAGE_SHIFT),
			NULL, &uk_fdio_vma_ops, &args, NULL);
	uk_ofile_release(description);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		return;
	for (unsigned int step = 0; step < 3; step++) {
		unsigned int index = order[step];
		__vaddr_t page = address + index * UK_PAGING_PAGE_SIZE;
		__u32 value = 0;

		UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, page,
							 UK_VMA_FAULT_WRITE));
		UK_TEST_EXPECT_ZERO(uk_nofault_cmpxchg32((__u32 *)page,
				&value, 0xabc000 + index, UK_NOFAULTF_NOPAGING));
		UK_TEST_EXPECT_SNUM_EQ(*(__u32 *)shared_buffers[index],
				       0xabc000 + index);
	}
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, address + UK_PAGING_PAGE_SIZE,
		UK_PAGING_PAGE_SIZE, UK_PAGING_PAGE_ATTR_PROT_READ, 0));
	for (unsigned int pass = 0; pass < 2; pass++) {
		UK_TEST_EXPECT_ZERO(uk_vma_advise(vas, address,
				sizeof(shared_buffers), UK_VMA_ADV_DONTNEED, 0));
		for (unsigned int step = 0; step < 3; step++) {
			unsigned int index = order[step];
			__vaddr_t page = address + index * UK_PAGING_PAGE_SIZE;
			__u32 value = 0;

			UK_TEST_EXPECT_ZERO(uk_vma_populate_page(vas, page,
								 UK_VMA_FAULT_READ));
			UK_TEST_EXPECT_ZERO(uk_nofault_read32((__u32 *)page,
						 &value, UK_NOFAULTF_NOPAGING));
			UK_TEST_EXPECT_SNUM_EQ(value, 0xabc000 + index);
		}
		UK_TEST_EXPECT_SNUM_EQ(fixture.acquired, 3);
		UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas,
			address + UK_PAGING_PAGE_SIZE, UK_PAGING_PAGE_SIZE,
			UK_PAGING_PAGE_ATTR_PROT_RW, 0));
		UK_TEST_EXPECT(uk_vma_find(vas, address + UK_PAGING_PAGE_SIZE)->start
				== address);
	}
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address, sizeof(shared_buffers), 0));
	UK_TEST_EXPECT_SNUM_EQ(fixture.released, 3);
	UK_TEST_EXPECT_ZERO(fixture.invalid_release);
	for (unsigned int index = 0; index < 3; index++)
		UK_TEST_EXPECT_ZERO(fixture.held[index]);
}
