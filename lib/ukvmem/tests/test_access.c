/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/test.h>
#include <uk/vmem.h>

UK_TESTCASE(ukvmem_access, present_page_respects_vma_permissions)
{
	const unsigned long attributes[] = {
		UK_PAGING_PAGE_ATTR_PROT_READ,
		UK_PAGING_PAGE_ATTR_PROT_NONE,
		UK_PAGING_PAGE_ATTR_PROT_READ | UK_PAGING_PAGE_ATTR_PROT_EXEC,
		UK_PAGING_PAGE_ATTR_PROT_RWX
	};
	const unsigned int access[] = {
		UK_VMA_FAULT_READ, UK_VMA_FAULT_WRITE, UK_VMA_FAULT_EXEC
	};
	const unsigned long permission[] = {
		UK_PAGING_PAGE_ATTR_PROT_READ, UK_PAGING_PAGE_ATTR_PROT_WRITE,
		UK_PAGING_PAGE_ATTR_PROT_EXEC
	};
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	int rc;

	rc = uk_vma_map_anon(vas, &address, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW, 0, NULL);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		return;
	rc = uk_vma_populate_page(vas, address, UK_VMA_FAULT_WRITE);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		goto out;
	*(unsigned long *)address = 0x5a62;
	for (unsigned int index = 0; index < ARRAY_SIZE(attributes); index++) {
		rc = uk_vma_set_attr(vas, address, UK_PAGING_PAGE_SIZE,
				     attributes[index], 0);
		UK_TEST_EXPECT_ZERO(rc);
		if (rc)
			continue;
		for (unsigned int type = 0; type < ARRAY_SIZE(access); type++) {
			int expected = attributes[index] & permission[type] ?
				0 : -EFAULT;

			rc = uk_vma_populate_page(vas, address, access[type]);
			UK_TEST_EXPECT_SNUM_EQ(rc, expected);
		}
	}
	rc = uk_vma_set_attr(vas, address, UK_PAGING_PAGE_SIZE,
			     UK_PAGING_PAGE_ATTR_PROT_RW, 0);
	UK_TEST_EXPECT_ZERO(rc);
	if (!rc) {
		rc = uk_vma_populate_page(vas, address, UK_VMA_FAULT_READ);
		UK_TEST_EXPECT_ZERO(rc);
		if (!rc)
			UK_TEST_EXPECT(*(unsigned long *)address == 0x5a62);
	}
out:
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address, UK_PAGING_PAGE_SIZE, 0));
}

uk_testsuite_register(ukvmem_access, NULL);
