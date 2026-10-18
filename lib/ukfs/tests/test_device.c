/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/errptr.h>
#include <uk/fs/device.h>
#include <uk/test.h>

static void *reject_allocation(struct uk_alloc *allocator __unused,
			       size_t size __unused)
{
	return NULL;
}

UK_TESTCASE(ukfs_device, retained_identity_is_not_reused)
{
	struct uk_fs_device *first = uk_fs_device_new(uk_alloc_get_default());
	struct uk_fs_device *second;
	__u32 minor;

	UK_TEST_EXPECT(!PTRISERR(first));
	if (PTRISERR(first))
		return;
	minor = uk_fs_device_minor(first);
	uk_fs_device_acquire(first);
	uk_fs_device_release(first);
	second = uk_fs_device_new(uk_alloc_get_default());
	UK_TEST_EXPECT(!PTRISERR(second));
	if (!PTRISERR(second)) {
		UK_TEST_EXPECT(uk_fs_device_minor(second) != minor);
		uk_fs_device_release(second);
	}
	UK_TEST_EXPECT(uk_fs_device_minor(first) == minor);
	uk_fs_device_release(first);
	second = uk_fs_device_new(uk_alloc_get_default());
	UK_TEST_EXPECT(!PTRISERR(second));
	if (!PTRISERR(second)) {
		UK_TEST_EXPECT(uk_fs_device_minor(second) == minor);
		uk_fs_device_release(second);
	}
}

UK_TESTCASE(ukfs_device, allocation_failure_has_a_typed_error)
{
	struct uk_alloc allocator = {.malloc = reject_allocation};
	struct uk_fs_device *device = uk_fs_device_new(&allocator);

	UK_TEST_EXPECT(PTRISERR(device));
	UK_TEST_EXPECT(PTR2ERR(device) == -ENOMEM);
}

uk_testsuite_register(ukfs_device, NULL);
