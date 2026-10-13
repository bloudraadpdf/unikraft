/* SPDX-License-Identifier: BSD-3-Clause */

#include <errno.h>
#include <string.h>
#include <uk/arch.h>
#include <uk/random/driver.h>
#include <uk/sglist.h>
#include <uk/spinlock.h>
#include <virtio/virtio_bus.h>
#include <virtio/virtio_ids.h>

static struct uk_alloc *allocator;
static struct virtqueue *entropy_queue;
static struct uk_spinlock entropy_lock = UK_SPINLOCK_INITIALIZER();
static __u8 entropy_buffer[64] __align(64);

static int entropy_bytes(__u8 *buffer, __sz size)
{
	struct uk_sglist_seg segment;
	struct uk_sglist list;
	unsigned long flags;
	void *completed;
	__u32 received;
	__sz requested;
	int rc = 0;

	if (!entropy_queue)
		return -ENODEV;
	uk_spin_lock_irqsave(&entropy_lock, flags);
	while (size) {
		requested = MIN(size, sizeof(entropy_buffer));
		uk_sglist_init(&list, 1, &segment);
		rc = uk_sglist_append(&list, entropy_buffer, requested);
		if (rc)
			break;
		rc = virtqueue_buffer_enqueue(entropy_queue, entropy_buffer, &list, 0, 1);
		if (rc < 0)
			break;
		virtqueue_host_notify(entropy_queue);
		/* Restore reseeding runs with IRQs disabled. */
		do {
			rc = virtqueue_buffer_dequeue(entropy_queue, &completed, &received);
			if (rc == -ENOMSG)
				uk_arch_spinwait();
		} while (rc == -ENOMSG);
		if (rc < 0)
			break;
		if (completed != entropy_buffer || !received || received > requested) {
			rc = -EIO;
			break;
		}
		memcpy(buffer, entropy_buffer, received);
		memset(entropy_buffer, 0, sizeof(entropy_buffer));
		buffer += received;
		size -= received;
		rc = 0;
	}
	uk_spin_unlock_irqrestore(&entropy_lock, flags);
	return rc;
}

static struct uk_random_driver_ops entropy_ops = {
	.random_bytes = entropy_bytes,
	.seed_bytes = entropy_bytes,
	.seed_bytes_fb = entropy_bytes,
};

static struct uk_random_driver entropy_source = {
	.name = "virtio-rng",
	.ops = &entropy_ops,
};

static int entropy_add_device(struct virtio_dev *device)
{
	__u64 features;
	__u16 descriptors;
	int rc;

	if (entropy_queue)
		return -EEXIST;
	features = virtio_feature_get(device);
	if (!VIRTIO_FEATURE_HAS(features, VIRTIO_F_VERSION_1))
		return -ENOTSUP;
	device->features = 0;
	VIRTIO_FEATURE_SET(device->features, VIRTIO_F_VERSION_1);
	virtio_feature_set(device);
	rc = virtio_dev_status_update(device, VIRTIO_CONFIG_STATUS_ACK |
		VIRTIO_CONFIG_STATUS_DRIVER | VIRTIO_CONFIG_STATUS_FEATURES_OK);
	if (rc)
		return rc;
	rc = virtio_find_vqs(device, 1, &descriptors);
	if (rc != 1 || !descriptors)
		return -ENODEV;
	entropy_queue = virtio_vqueue_setup(device, 0, MIN(descriptors, (__u16)16),
		NULL, allocator);
	if (PTRISERR(entropy_queue)) {
		rc = PTR2ERR(entropy_queue);
		entropy_queue = NULL;
		return rc;
	}
	virtqueue_intr_disable(entropy_queue);
	rc = virtio_dev_status_update(device,
		virtio_dev_status_get(device) | VIRTIO_CONFIG_STATUS_DRIVER_OK);
	if (!rc)
		rc = uk_random_init(&entropy_source);
	if (rc) {
		virtio_dev_status_update(device,
			virtio_dev_status_get(device) | VIRTIO_CONFIG_STATUS_FAIL);
		virtio_vqueue_release(device, entropy_queue, allocator);
		entropy_queue = NULL;
	}
	return rc;
}

static int entropy_driver_init(struct uk_alloc *driver_allocator)
{
	if (!driver_allocator)
		return -EINVAL;
	allocator = driver_allocator;
	return 0;
}

static const struct virtio_dev_id entropy_devices[] = {
	{ VIRTIO_ID_RNG },
	{ VIRTIO_ID_INVALID },
};

static struct virtio_driver entropy_driver = {
	.dev_ids = entropy_devices,
	.init = entropy_driver_init,
	.add_dev = entropy_add_device,
};

VIRTIO_BUS_REGISTER_DRIVER(&entropy_driver);
