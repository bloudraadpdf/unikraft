/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UKFS_FS_DEVICE_H__
#define __UKFS_FS_DEVICE_H__

#include <uk/alloc.h>
#include <uk/arch/types.h>

struct uk_fs_device;

struct uk_fs_device *uk_fs_device_new(struct uk_alloc *allocator);
void uk_fs_device_acquire(struct uk_fs_device *device);
void uk_fs_device_release(struct uk_fs_device *device);
__u32 uk_fs_device_minor(const struct uk_fs_device *device);

#endif /* __UKFS_FS_DEVICE_H__ */
