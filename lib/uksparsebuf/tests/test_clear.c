/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include <uk/sparsebuf.h>
#include <uk/test.h>

#define SLICE_COUNT 32

static struct uk_sparsebuf_blk blocks[SLICE_COUNT];
static unsigned char buffers[SLICE_COUNT][UK_PAGING_PAGE_SIZE];
static unsigned int allocated, freed, released;
static int invalid_release;

static void *block_alloc(struct uk_alloc *allocator __unused, __sz size)
{
	if (size != sizeof(blocks[0]) || allocated == SLICE_COUNT)
		return NULL;
	return &blocks[allocated++];
}

static void block_free(struct uk_alloc *allocator __unused, void *block)
{
	memset(block, 0, sizeof(blocks[0]));
	freed++;
}

static void *buffer_alloc(__u64 offset, __u32 pages, void *arg __unused)
{
	UK_ASSERT(offset / 2 < SLICE_COUNT && pages == 1);
	return buffers[offset / 2];
}

static void buffer_free(void *buffer, __u64 offset, __u32 pages,
			void *arg __unused)
{
	if (offset / 2 >= SLICE_COUNT || pages != 1 ||
	    buffer != buffers[offset / 2])
		invalid_release = 1;
	released++;
}

UK_TESTCASE(uksparsebuf, clear_unlinks_before_metadata_release)
{
	struct uk_alloc allocator = {.malloc = block_alloc, .free = block_free};
	const struct uk_sparsebuf_alloc_funcs functions = {
		.alloc_new = buffer_alloc, .alloc_free = buffer_free
	};
	const struct uk_sparsebuf_ctx context = {
		.alloc = &allocator, .funcs = &functions
	};
	struct uk_sparsebuf_blk *head = NULL;

	allocated = freed = released = 0;
	invalid_release = 0;
	for (unsigned int index = 0; index < SLICE_COUNT; index++) {
		__ssz rc = uk_sparsebuf_fill(&context, &head, 2 * index, 1, 0);

		UK_TEST_EXPECT_SNUM_EQ(rc, 1);
		if (rc != 1)
			break;
	}
	uk_sparsebuf_clear(&context, &head);
	UK_TEST_EXPECT_NULL(head);
	UK_TEST_EXPECT_SNUM_EQ(freed, SLICE_COUNT);
	UK_TEST_EXPECT_SNUM_EQ(released, SLICE_COUNT);
	UK_TEST_EXPECT_ZERO(invalid_release);
}

uk_testsuite_register(uksparsebuf, NULL);
