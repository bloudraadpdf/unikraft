/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/test.h>
#include <uk/vmem.h>

static struct uk_vma_anon_identity *identity_at(struct uk_vas *vas,
						__vaddr_t address, __sz *offset)
{
	struct uk_vma_anon_identity *identity;

	uk_mutex_lock(&vas->mutation_lock);
	identity = uk_vma_anon_identity_acquire(uk_vma_find(vas, address),
						address, offset);
	uk_mutex_unlock(&vas->mutation_lock);
	return identity;
}

UK_TESTCASE(ukvmem_anon, identity_survives_split_unmap_and_replacement)
{
	struct uk_vas *vas = uk_vas_get_active();
	struct uk_vma_anon_identity *first, *split, *second, *replacement;
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__vaddr_t next, middle;
	__sz offset;
	int rc;

	rc = uk_vma_reserve(vas, &address, 6 * UK_PAGING_PAGE_SIZE);
	UK_TEST_EXPECT_ZERO(rc);
	next = address + 3 * UK_PAGING_PAGE_SIZE;
	rc = uk_vma_map_anon(vas, &address, 3 * UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW,
			     UK_VMA_MAP_REPLACE | UK_VMA_ANON_SHARED, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	rc = uk_vma_map_anon(vas, &next, 3 * UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW,
			     UK_VMA_MAP_REPLACE | UK_VMA_ANON_SHARED, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	first = identity_at(vas, address, &offset);
	UK_TEST_EXPECT_NOT_NULL(first);
	UK_TEST_EXPECT_ZERO(offset);
	second = identity_at(vas, next, &offset);
	UK_TEST_EXPECT_NOT_NULL(second);
	UK_TEST_EXPECT(first != second);
	UK_TEST_EXPECT(uk_vma_find(vas, address)->end == next);
	middle = address + UK_PAGING_PAGE_SIZE;
	rc = uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_READ, 0);
	UK_TEST_EXPECT_ZERO(rc);
	split = identity_at(vas, middle + 16, &offset);
	UK_TEST_EXPECT(split == first);
	UK_TEST_EXPECT(offset == UK_PAGING_PAGE_SIZE + 16);
	uk_vma_anon_identity_release(split);
	rc = uk_vma_set_attr(vas, middle, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW, 0);
	UK_TEST_EXPECT_ZERO(rc);
	UK_TEST_EXPECT(uk_vma_find(vas, address)->end == next);
	UK_TEST_EXPECT(uk_vma_find(vas, middle)->start == address);
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
					 6 * UK_PAGING_PAGE_SIZE, 0));
	rc = uk_vma_map_anon(vas, &address, 3 * UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW,
			     UK_VMA_ANON_SHARED, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	replacement = identity_at(vas, address, &offset);
	UK_TEST_EXPECT_NOT_NULL(replacement);
	UK_TEST_EXPECT(replacement != first && replacement != second);
	UK_TEST_EXPECT_ZERO(offset);
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
					 3 * UK_PAGING_PAGE_SIZE, 0));
	uk_vma_anon_identity_release(replacement);
	uk_vma_anon_identity_release(second);
	uk_vma_anon_identity_release(first);
}

UK_TESTCASE(ukvmem_anon, private_mappings_keep_coalescing_without_identity)
{
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__vaddr_t next;
	__sz offset = 91;
	int rc;

	UK_TEST_EXPECT_ZERO(uk_vma_reserve(vas, &address,
					   2 * UK_PAGING_PAGE_SIZE));
	next = address + UK_PAGING_PAGE_SIZE;
	rc = uk_vma_map_anon(vas, &address, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW,
			     UK_VMA_MAP_REPLACE, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	rc = uk_vma_map_anon(vas, &next, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW,
			     UK_VMA_MAP_REPLACE, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	UK_TEST_EXPECT(uk_vma_find(vas, next)->start == address);
	UK_TEST_EXPECT_NULL(identity_at(vas, address, &offset));
	UK_TEST_EXPECT(offset == 91);
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
					 2 * UK_PAGING_PAGE_SIZE, 0));
}

uk_testsuite_register(ukvmem_anon, NULL);
