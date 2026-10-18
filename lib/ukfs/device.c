/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/errptr.h>
#include <uk/fs/device.h>
#include <uk/list.h>
#include <uk/refcount.h>
#include <uk/rwlock.h>

#define UK_FS_DEVICE_DYNAMIC_MINOR_MIN 256
#define UK_FS_DEVICE_MINOR_MAX ((1U << 20) - 1)

struct uk_fs_device {
	struct uk_list_head link;
	struct uk_alloc *allocator;
	__atomic references;
	__u32 minor;
};

static UK_LIST_HEAD(devices);
static struct uk_rwlock device_lock = UK_RWLOCK_INITIALIZER(device_lock, 0);

struct uk_fs_device *uk_fs_device_new(struct uk_alloc *allocator)
{
	struct uk_fs_device *device = uk_malloc(allocator, sizeof(*device));
	struct uk_list_head *entry;
	__u32 minor = UK_FS_DEVICE_DYNAMIC_MINOR_MIN;

	if (!device)
		return ERR2PTR(-ENOMEM);
	uk_rwlock_wlock(&device_lock);
	uk_list_for_each(entry, &devices) {
		struct uk_fs_device *used =
			uk_list_entry(entry, struct uk_fs_device, link);

		if (used->minor != minor)
			break;
		minor++;
	}
	if (minor > UK_FS_DEVICE_MINOR_MAX) {
		uk_rwlock_wunlock(&device_lock);
		uk_free(allocator, device);
		return ERR2PTR(-ENOSPC);
	}
	device->allocator = allocator;
	device->minor = minor;
	uk_refcount_init(&device->references, 1);
	uk_list_add_tail(&device->link, entry);
	uk_rwlock_wunlock(&device_lock);
	return device;
}

void uk_fs_device_acquire(struct uk_fs_device *device)
{
	uk_refcount_acquire(&device->references);
}

void uk_fs_device_release(struct uk_fs_device *device)
{
	if (!uk_refcount_release(&device->references))
		return;
	uk_rwlock_wlock(&device_lock);
	uk_list_del(&device->link);
	uk_rwlock_wunlock(&device_lock);
	uk_free(device->allocator, device);
}

__u32 uk_fs_device_minor(const struct uk_fs_device *device)
{
	return device->minor;
}
