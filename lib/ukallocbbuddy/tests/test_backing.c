/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <string.h>
#include <uk/allocbbuddy.h>
#include <uk/arch/limits.h>
#include <uk/test.h>
#include <uk/essentials.h>

static unsigned char region[16 * __PAGE_SIZE] __align(__PAGE_SIZE);

struct backing_control {
	unsigned int calls;
	unsigned int fail_at;
	unsigned int releases;
	size_t largest_release;
	int invalid_release;
};

static int prepare(void *context, uintptr_t address __unused,
		   size_t length __unused)
{
	struct backing_control *control = context;

	control->calls++;
	return control->calls == control->fail_at ? -ENOMEM : 0;
}

static void release(void *context, uintptr_t address, size_t length)
{
	struct backing_control *control = context;

	if ((address & (__PAGE_SIZE - 1)) || (length & (__PAGE_SIZE - 1)) ||
	    address < (uintptr_t)region + __PAGE_SIZE ||
	    address + length > (uintptr_t)region + sizeof(region))
		control->invalid_release = 1;
	control->releases++;
	if (length > control->largest_release)
		control->largest_release = length;
}

UK_TESTCASE(ukallocbbuddy_backing, failure_keeps_free_lists)
{
	static struct backing_control control;
	struct uk_alloc *allocator = uk_allocbbuddy_init(region, sizeof(region));
	const struct uk_allocbbuddy_backing backing = {
		.prepare = prepare, .release = release, .context = &control
	};
	void *first, *second, *pages[8];
	long available;

	UK_TEST_EXPECT(allocator != NULL);
	if (!allocator)
		return;
	uk_allocbbuddy_set_backing(allocator, &backing);
	available = uk_alloc_pavailmem(allocator);
	for (unsigned int failure = 1; failure <= 2; failure++) {
		control.calls = control.releases = 0;
		control.fail_at = failure;
		first = uk_palloc(allocator, 3);
		UK_TEST_EXPECT(first == NULL);
		UK_TEST_EXPECT(control.calls == failure);
		UK_TEST_EXPECT(control.releases == 1);
		UK_TEST_EXPECT(uk_alloc_pavailmem(allocator) == available);
	}
	control.fail_at = 0;
	first = uk_palloc(allocator, 3);
	second = uk_palloc(allocator, 1);
	UK_TEST_EXPECT(first != NULL && second != NULL);
	if (first)
		memset(first, 0xa5, 3 * __PAGE_SIZE);
	if (second)
		memset(second, 0x5a, __PAGE_SIZE);
	if (first)
		uk_pfree(allocator, first, 3);
	if (second) {
		UK_TEST_EXPECT(*(unsigned char *)second == 0x5a);
		uk_pfree(allocator, second, 1);
	}
	UK_TEST_EXPECT(uk_alloc_pavailmem(allocator) == available);
	for (unsigned int i = 0; i < ARRAY_SIZE(pages); i++) {
		pages[i] = uk_palloc(allocator, 1);
		UK_TEST_EXPECT(pages[i] != NULL);
	}
	control.largest_release = 0;
	for (unsigned int i = ARRAY_SIZE(pages); i > 0; i--)
		if (pages[i - 1])
			uk_pfree(allocator, pages[i - 1], 1);
	UK_TEST_EXPECT(control.largest_release <= __PAGE_SIZE);
	UK_TEST_EXPECT(uk_alloc_pavailmem(allocator) == available);
	UK_TEST_EXPECT(!control.invalid_release);
}

uk_testsuite_register(ukallocbbuddy_backing, NULL);
