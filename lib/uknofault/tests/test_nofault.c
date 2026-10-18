/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <uk/nofault.h>
#include <uk/paging.h>
#include <uk/assert.h>
#include <uk/test.h>
#include <uk/essentials.h>

#define TEST_MAP_BASE	0x20000000000 /* 2 TiB - must not be used otherwise */
#define TEST_MAP_END	(TEST_MAP_BASE + UK_PAGING_PAGE_SIZE)

#define nf_bug_on(cond)							\
	do {								\
		if (unlikely(cond))					\
			UK_CRASH("'%s' during test execution.\n",	\
				 STRINGIFY(cond));			\
	} while (0)

UK_TESTCASE(uknofault, test_nofault_probe_noaccess)
{
	__sz len;

	len = uk_nofault_probe_r(TEST_MAP_BASE, 1, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 0);

	len = uk_nofault_probe_r(TEST_MAP_BASE, UK_PAGING_PAGE_SIZE, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 0);
}

/**
 * Tests if we can successfully probe for read access
 */
UK_TESTCASE(uknofault, test_nofault_probe_r)
{
	struct uk_pagetable *pt = uk_paging_pt_get_active();
	__sz len;
	int rc;

	rc = uk_paging_page_map(pt, TEST_MAP_BASE, UK_PAGING_PADDR_ANY, 1,
				UK_PAGING_PAGE_ATTR_PROT_READ, 0);
	nf_bug_on(rc != 0);

	len = uk_nofault_probe_r(TEST_MAP_BASE, 1, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1);

	len = uk_nofault_probe_r(TEST_MAP_BASE, UK_PAGING_PAGE_SIZE, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, UK_PAGING_PAGE_SIZE);

	len = uk_nofault_probe_r(TEST_MAP_BASE - 1, 2, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 0);

	len = uk_nofault_probe_r(TEST_MAP_BASE - 6, 8, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 0);

	len = uk_nofault_probe_r(TEST_MAP_END - 1, 8, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1);

	len = uk_nofault_probe_r(TEST_MAP_END - 1244, UK_PAGING_PAGE_SIZE, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1244);

	/* Map another page and leave a hole of one page to check if
	 * continuation works
	 */
	rc = uk_paging_page_map(pt, TEST_MAP_BASE + 2 * UK_PAGING_PAGE_SIZE,
				UK_PAGING_PADDR_ANY, 1,
				UK_PAGING_PAGE_ATTR_PROT_READ, 0);
	nf_bug_on(rc != 0);

	len = uk_nofault_probe_r(TEST_MAP_BASE, 3 * UK_PAGING_PAGE_SIZE,
				 UK_NOFAULTF_CONTINUE);
	UK_TEST_EXPECT_SNUM_EQ(len, 2 * UK_PAGING_PAGE_SIZE);

	/* Clean up */
	rc = uk_paging_page_unmap(pt, TEST_MAP_BASE, 3, 0);
	nf_bug_on(rc != 0);
}

/**
 * Tests if we can successfully probe for read/write access. This will also test
 * the uk_nofault_memcpy() function as the read/write probe functions use the
 * memcpy function.
 */
UK_TESTCASE(uknofault, test_nofault_probe_rw)
{
	struct uk_pagetable *pt = uk_paging_pt_get_active();
	__sz len;
	int rc;

	rc = uk_paging_page_map(pt, TEST_MAP_BASE,
				UK_PAGING_PADDR_ANY, 1,
				UK_PAGING_PAGE_ATTR_PROT_READ, 0);
	nf_bug_on(rc != 0);

	len = uk_nofault_probe_r(TEST_MAP_BASE, 1, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1);

	len = uk_nofault_probe_rw(TEST_MAP_BASE, 1, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 0);

	/* Make the page writeable. Set a sanity value so we can verify that
	 * the rw probe does not change memory contents
	 */
	rc = uk_paging_page_set_attr(pt, TEST_MAP_BASE, 1,
				     UK_PAGING_PAGE_ATTR_PROT_RW, 0);
	nf_bug_on(rc != 0);

	*((unsigned long *)TEST_MAP_BASE) = 0xdeadb0b0;

	len = uk_nofault_probe_rw(TEST_MAP_BASE, 1, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1);

	len = uk_nofault_probe_rw(TEST_MAP_END - 1244, UK_PAGING_PAGE_SIZE, 0);
	UK_TEST_EXPECT_SNUM_EQ(len, 1244);

	/* Map another page and leave a hole of one page to check if
	 * continuation works
	 */
	rc = uk_paging_page_map(pt, TEST_MAP_BASE + 2 * UK_PAGING_PAGE_SIZE,
				UK_PAGING_PADDR_ANY, 1,
				UK_PAGING_PAGE_ATTR_PROT_RW, 0);
	nf_bug_on(rc != 0);

	len = uk_nofault_probe_rw(TEST_MAP_BASE, 3 * UK_PAGING_PAGE_SIZE,
				  UK_NOFAULTF_CONTINUE);
	UK_TEST_EXPECT_SNUM_EQ(len, 2 * UK_PAGING_PAGE_SIZE);

	/* Check sanity value */
	UK_TEST_EXPECT_SNUM_EQ(*((unsigned long *)TEST_MAP_BASE), 0xdeadb0b0);

	/* Clean up */
	rc = uk_paging_page_unmap(pt, TEST_MAP_BASE, 3, 0);
	nf_bug_on(rc != 0);
}

/**
 * Tests the uk_nofault_try_read function
 */
UK_TESTCASE(uknofault, test_nofault_try_read)
{
	unsigned long v = 0x42;
	unsigned long v2;
	int rc;

	rc = uk_nofault_try_read(v2, UK_PAGING_VADDR_INV);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);

	rc = uk_nofault_try_read(v2, &v);
	UK_TEST_EXPECT_SNUM_EQ(rc, 1);
	UK_TEST_EXPECT_SNUM_EQ(v2, v);
}

/**
 * Tests the uk_nofault_try_write function
 */
UK_TESTCASE(uknofault, test_nofault_try_write)
{
	unsigned long v = 0x42;
	unsigned long v2;
	int rc;

	rc = uk_nofault_try_write(v, UK_PAGING_VADDR_INV);
	UK_TEST_EXPECT_SNUM_EQ(rc, 0);

	rc = uk_nofault_try_write(v, &v2);
	UK_TEST_EXPECT_SNUM_EQ(rc, 1);
	UK_TEST_EXPECT_SNUM_EQ(v2, v);
}

UK_TESTCASE(uknofault, atomic_word_access_preserves_values_on_fault)
{
	__u32 word = 17, value = 99, expected = 4;
	__u32 *invalid = (__u32 *)UK_PAGING_VADDR_INV;
	int result;

	result = uk_nofault_read32(&word, &value, UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(value, 17);
	result = uk_nofault_cmpxchg32(&word, &expected, 23,
				      UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(expected, 17);
	UK_TEST_EXPECT_SNUM_EQ(word, 17);
	result = uk_nofault_cmpxchg32(&word, &expected, 23,
				      UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(word, 23);
	expected = 23;
	result = uk_nofault_cmpxchg32(&word, &expected, 31, 0);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(word, 31);
	value = 99;
	result = uk_nofault_read32(invalid, &value, UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(value, 99);
	result = uk_nofault_cmpxchg32(invalid, &expected, 0,
				      UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(expected, 23);
	result = uk_nofault_read32(&word, &value, UK_NOFAULTF_CONTINUE);
	UK_TEST_EXPECT_SNUM_EQ(result, -EINVAL);
	UK_TEST_EXPECT_SNUM_EQ(value, 99);
	result = uk_nofault_cmpxchg32(&word, &expected, 0,
				      UK_NOFAULTF_CONTINUE);
	UK_TEST_EXPECT_SNUM_EQ(result, -EINVAL);
	UK_TEST_EXPECT_SNUM_EQ(word, 31);
}

#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>

UK_TESTCASE(uknofault, atomic_nopaging_does_not_populate_a_lazy_mapping)
{
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__u32 value = 99, expected = 0;
	__u32 *word;
	unsigned long flags = vas->flags;
	int result;

	result = uk_vma_map(vas, &address, UK_PAGING_PAGE_SIZE,
			    UK_PAGING_PAGE_ATTR_PROT_RW, 0, NULL,
			    &uk_vma_anon_ops, NULL, NULL);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (result)
		return;
	word = (__u32 *)address;
	result = uk_nofault_read32(word, &value, UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, -EFAULT);
	UK_TEST_EXPECT_SNUM_EQ(value, 99);
	UK_TEST_EXPECT_SNUM_EQ(vas->flags, flags);
	result = uk_nofault_read32(word, &value, 0);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(value, 0);
	result = uk_nofault_cmpxchg32(word, &expected, 9, UK_NOFAULTF_NOPAGING);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_SNUM_EQ(vas->flags, flags);
	result = uk_vma_set_attr(vas, address, UK_PAGING_PAGE_SIZE,
				 UK_PAGING_PAGE_ATTR_PROT_READ, 0);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!result) {
		expected = 9;
		result = uk_nofault_read32(word, &value, UK_NOFAULTF_NOPAGING);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		UK_TEST_EXPECT_SNUM_EQ(value, 9);
		result = uk_nofault_cmpxchg32(word, &expected, 10,
					      UK_NOFAULTF_NOPAGING);
		UK_TEST_EXPECT_SNUM_EQ(result, -EFAULT);
		UK_TEST_EXPECT_SNUM_EQ(expected, 9);
		result = uk_nofault_read32(word, &value, 0);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		UK_TEST_EXPECT_SNUM_EQ(value, 9);
	}
	UK_TEST_EXPECT_SNUM_EQ(vas->flags, flags);
	UK_TEST_EXPECT_SNUM_EQ(uk_vma_unmap(vas, address,
					    UK_PAGING_PAGE_SIZE, 0), 0);
}
#endif

uk_testsuite_register(uknofault, NULL);
