/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/nofault.h>
#include <uk/test.h>
#include <uk/vmem.h>

UK_TESTCASE(ukvmem_stack, split_guards_growth_and_cleanup)
{
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__vaddr_t other = UK_PAGING_VADDR_ANY;
	__vaddr_t start, end, bottom, middle;
	unsigned int value = 7;
	int rc;

	rc = uk_vma_map_stack(vas, &address, 24 * UK_PAGING_PAGE_SIZE,
			      0, "stack-lifetime", 0);
	UK_TEST_EXPECT_ZERO(rc);
	rc = uk_vma_map_stack(vas, &other, 16 * UK_PAGING_PAGE_SIZE,
			      0, "stack-lifetime", 0);
	UK_TEST_EXPECT_ZERO(rc);
	rc = uk_vma_stack_resize(vas, address,
				 address + 8 * UK_PAGING_PAGE_SIZE);
	UK_TEST_EXPECT_ZERO(rc);
	address += 8 * UK_PAGING_PAGE_SIZE;
	vma = uk_vma_find(vas, address);
	UK_TEST_EXPECT_ZERO(uk_vma_stack_bounds(vma, &start, &end));
	middle = address + 4 * UK_PAGING_PAGE_SIZE;
	rc = uk_vma_set_attr(vas, middle, 3 * UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RWX,
			     UK_VMA_FLAG_STRICT_VMA_CHECK);
	UK_TEST_EXPECT_ZERO(rc);
	UK_TEST_EXPECT_NOT_ZERO(uk_nofault_try_write(value,
						     (unsigned int *)middle));
	rc = uk_nofault_try_write(value,
				  (unsigned int *)(middle +
					    2 * UK_PAGING_PAGE_SIZE));
	UK_TEST_EXPECT_NOT_ZERO(rc);
	UK_TEST_EXPECT_ZERO(uk_nofault_probe_r(start, 1, 0));
	UK_TEST_EXPECT_ZERO(uk_nofault_probe_r(end - 1, 1, 0));
	bottom = address - 2 * UK_PAGING_PAGE_SIZE;
	UK_TEST_EXPECT_ZERO(uk_vma_stack_resize(vas, address, bottom));
	vma = uk_vma_find(vas, bottom);
	UK_TEST_EXPECT_SNUM_EQ(vma->attr, UK_PAGING_PAGE_ATTR_PROT_RW);
	UK_TEST_EXPECT_NOT_ZERO(uk_nofault_try_write(value,
						     (unsigned int *)bottom));
	UK_TEST_EXPECT_ZERO(uk_nofault_probe_r(bottom - 1, 1, 0));
	UK_TEST_EXPECT_SNUM_EQ(uk_vma_find(vas, middle)->attr,
			       UK_PAGING_PAGE_ATTR_PROT_RWX);
	rc = uk_vma_set_attr(vas, bottom, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_READ,
			     UK_VMA_FLAG_STRICT_VMA_CHECK);
	UK_TEST_EXPECT_ZERO(rc);
	bottom -= 2 * UK_PAGING_PAGE_SIZE;
	UK_TEST_EXPECT_ZERO(uk_vma_stack_resize(vas, address, bottom));
	vma = uk_vma_find(vas, bottom);
	UK_TEST_EXPECT_SNUM_EQ(vma->attr, UK_PAGING_PAGE_ATTR_PROT_READ);
	UK_TEST_EXPECT_SNUM_EQ(uk_nofault_probe_r(bottom, sizeof(value), 0),
			       sizeof(value));
	UK_TEST_EXPECT_ZERO(uk_nofault_try_write(value,
						 (unsigned int *)bottom));
	UK_TEST_EXPECT_ZERO(uk_nofault_probe_r(bottom - 1, 1, 0));
	UK_TEST_EXPECT_ZERO(uk_vma_stack_unmap(vas, address));
	UK_TEST_EXPECT_NULL(uk_vma_find(vas, bottom));
	UK_TEST_EXPECT_NULL(uk_vma_find(vas, address));
	UK_TEST_EXPECT_NULL(uk_vma_find(vas, middle));
	UK_TEST_EXPECT_NULL(uk_vma_find(vas, end - 1));
	UK_TEST_EXPECT_ZERO(uk_nofault_probe_r(middle, 1, 0));
	UK_TEST_EXPECT_NOT_NULL(uk_vma_find(vas, other));
	UK_TEST_EXPECT_ZERO(uk_vma_stack_unmap(vas, other));
}

uk_testsuite_register(ukvmem_stack, NULL);
