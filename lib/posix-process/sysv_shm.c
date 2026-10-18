/* SPDX-License-Identifier: BSD-3-Clause */
#define _GNU_SOURCE
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <uk/fs.h>
#include <uk/fs/driver.h>
#include <uk/posix-fdio.h>
#include <uk/posix-file-vma.h>
#include <uk/vmem.h>
#include "sysv.h"

#define SHM_RDONLY 010000
#define SHM_RND 020000
#define SHM_REMAP 040000
#define SHM_EXEC 0100000
#define SHM_DEST 01000
#define SHM_LOCKED 02000
#define SHM_LOCK 11
#define SHM_UNLOCK 12
#define SHM_STAT 13
#define SHM_STAT_ANY 15

struct sysv_shmid {
	struct sysv_perm perm;
	unsigned long size;
	long atime, dtime, ctime;
	int cpid, lpid;
	unsigned long nattch;
	unsigned long unused[2];
};

UK_CTASSERT(sizeof(struct sysv_shmid) == 112);

struct shm_segment {
	struct sysv_object object;
	struct sysv_shmid status;
	struct uk_ofile *file;
};

struct shm_attachment {
	struct uk_fdio_vma_lifetime lifetime;
	struct uk_list_head link;
	struct posix_vm_context *vm;
	struct shm_segment *segment;
	__vaddr_t base;
	unsigned int references;
	unsigned int mappings;
	bool retired;
	pid_t owner_pid;
	pid_t detaching_pid;
};

static UK_LIST_HEAD(attachments);

static void shm_destroy(struct sysv_object *object)
{
	struct shm_segment *segment = (struct shm_segment *)object;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_ofile_release(segment->file);
	uk_free(uk_alloc_get_default(), segment);
}

static struct uk_ofile *shm_file(size_t size)
{
	const struct uk_fs_drv *driver = uk_fs_driver("ramfs");
	const struct uk_file *root, *file;
	struct uk_ofile *open;
	int rc;

	if (!driver)
		return ERR2PTR(-ENODEV);
	root = driver->vopen(UK_FS_VOPEN_NULLVOL, 0, UK_FS_VOPEN_NULLDATA, 0);
	if (PTRISERR(root))
		return (void *)root;
	uk_file_wlock(root);
	file = uk_fs_createat(root, NULL, 0, S_IFREG | 0600, O_TMPFILE,
			      UKFS_NOTARGET);
	uk_file_wunlock(root);
	uk_file_release(root);
	if (PTRISERR(file))
		return (void *)file;
	open = uk_ofile_new(file, O_RDWR, 0);
	uk_file_release(file);
	if (!open)
		return ERR2PTR(-ENOMEM);
	rc = uk_sys_ftruncate(open, size);
	if (rc) {
		uk_ofile_release(open);
		return ERR2PTR(rc);
	}
	return open;
}

static int shm_existing_result(struct sysv_object *object, size_t size)
{
	if (PTRISERR(object))
		return PTR2ERR(object);
	return size > ((struct shm_segment *)object)->status.size ? -EINVAL
								  : object->id;
}

UK_LLSYSCALL_R_DEFINE(int, shmget, int, key, size_t, size, int, flags)
{
	struct sysv_object *object;
	struct shm_segment *segment;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_key(SYSV_SHM, key, flags);
	if (object) {
		rc = shm_existing_result(object, size);
		uk_mutex_unlock(&pprocess_mutex);
		return rc;
	}
	uk_mutex_unlock(&pprocess_mutex);
	if (!size || size > LONG_MAX - UK_PAGING_PAGE_SIZE)
		return -EINVAL;
	if (flags & 04000)
		return -EOPNOTSUPP;
	segment = uk_zalloc(uk_alloc_get_default(), sizeof(*segment));
	if (!segment)
		return -ENOMEM;
	segment->file = shm_file(size);
	if (PTRISERR(segment->file)) {
		rc = PTR2ERR(segment->file);
		uk_free(uk_alloc_get_default(), segment);
		return rc;
	}
	segment->object.destroy = shm_destroy;
	segment->status.size = size;
	segment->status.ctime = sysv_now();
	segment->status.cpid = uk_pprocess_current()->pid;
	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_key(SYSV_SHM, key, flags);
	if (object) {
		rc = shm_existing_result(object, size);
	} else {
		rc = sysv_insert(&segment->object, SYSV_SHM, key, flags);
		if (rc >= 0)
			segment = NULL;
	}
	uk_mutex_unlock(&pprocess_mutex);
	if (segment)
		shm_destroy(&segment->object);
	return rc;
}

static void shm_segment_put(struct shm_segment *segment)
{
	bool release;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(segment->object.references);
	release = !--segment->object.references;
	uk_mutex_unlock(&pprocess_mutex);
	if (release)
		shm_destroy(&segment->object);
}

static struct shm_attachment *attachment_put_locked(struct shm_attachment *a)
{
	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	UK_ASSERT(a->references);
	if (--a->references)
		return NULL;
	UK_ASSERT(!a->mappings);
	uk_list_del(&a->link);
	return a;
}

static void attachment_destroy(struct shm_attachment *attachment)
{
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	if (!attachment)
		return;
	shm_segment_put(attachment->segment);
	uk_free(uk_alloc_get_default(), attachment);
}

static void shm_mapping_acquire(struct uk_fdio_vma_lifetime *lifetime)
{
	struct shm_attachment *attachment = __containerof(lifetime,
					struct shm_attachment, lifetime);

	uk_mutex_lock(&pprocess_mutex);
	attachment->references++;
	attachment->mappings++;
	if (!attachment->retired)
		attachment->segment->status.nattch++;
	uk_mutex_unlock(&pprocess_mutex);
}

static void shm_detach_count_locked(struct shm_attachment *attachment,
				    unsigned int count, pid_t pid)
{
	struct shm_segment *segment = attachment->segment;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	UK_ASSERT(segment->status.nattch >= count);
	if (!count)
		return;
	segment->status.nattch -= count;
	segment->status.dtime = sysv_now();
	segment->status.lpid = pid;
	if (!segment->status.nattch && !segment->object.removed &&
	    (segment->object.perm.mode & SHM_DEST))
		sysv_remove(&segment->object);
}

static void shm_mapping_release(struct uk_fdio_vma_lifetime *lifetime)
{
	struct shm_attachment *attachment = __containerof(lifetime,
					struct shm_attachment, lifetime);
	struct shm_attachment *dead;
	pid_t pid;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(attachment->mappings);
	attachment->mappings--;
	if (!attachment->retired) {
		pid = attachment->detaching_pid ? attachment->detaching_pid
		      : uk_pthread_current()	? uk_pprocess_current()->pid
						: attachment->owner_pid;
		shm_detach_count_locked(attachment, 1, pid);
	}
	dead = attachment_put_locked(attachment);
	uk_mutex_unlock(&pprocess_mutex);
	attachment_destroy(dead);
}

UK_LLSYSCALL_R_DEFINE(long, shmat, int, id, const void *, address, int, flags)
{
	struct sysv_object *object;
	struct shm_segment *segment;
	struct shm_attachment *attachment, *dead = NULL;
	struct uk_ofile *mapping = NULL;
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t base = address ? (__vaddr_t)address : UK_PAGING_VADDR_ANY;
	unsigned long attr = UK_PAGING_PAGE_ATTR_PROT_READ;
	struct uk_fdio_vma_args mapping_args;
	unsigned long map_flags = UK_VMA_FILE_SHARED;
	unsigned int mode = 4;
	size_t length;
	long rc;

	if (flags & ~(SHM_RDONLY | SHM_RND | SHM_REMAP | SHM_EXEC))
		return -EINVAL;
	if (address && !UK_PAGING_PAGE_ALIGNED(base)) {
		if (!(flags & SHM_RND))
			return -EINVAL;
		base = ALIGN_DOWN(base, UK_PAGING_PAGE_SIZE);
	}
	if (flags & SHM_REMAP) {
		if (!address)
			return -EINVAL;
		map_flags |= UK_VMA_MAP_REPLACE;
	}
	if (!(flags & SHM_RDONLY)) {
		attr |= UK_PAGING_PAGE_ATTR_PROT_WRITE;
		mode |= 2;
	}
	if (flags & SHM_EXEC) {
		attr |= UK_PAGING_PAGE_ATTR_PROT_EXEC;
		mode |= 1;
	}
	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&vas->mutation_lock);
	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_id(SYSV_SHM, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	rc = sysv_access(object, mode);
	if (rc)
		goto out;
	segment = (struct shm_segment *)object;
	mapping = uk_ofile_new(segment->file->file,
			       flags & SHM_RDONLY ? O_RDONLY : O_RDWR, 0);
	if (!mapping) {
		rc = -ENOMEM;
		goto out;
	}
	attachment = uk_zalloc(uk_alloc_get_default(), sizeof(*attachment));
	if (!attachment) {
		rc = -ENOMEM;
		goto out;
	}
	attachment->lifetime = (struct uk_fdio_vma_lifetime) {
		.acquire = shm_mapping_acquire,
		.release = shm_mapping_release,
	};
	attachment->vm = uk_pprocess_current()->vm_context;
	attachment->segment = segment;
	attachment->references = 1;
	attachment->owner_pid = uk_pprocess_current()->pid;
	object->references++;
	uk_list_add_tail(&attachment->link, &attachments);
	length = ALIGN_UP(segment->status.size, UK_PAGING_PAGE_SIZE);
	mapping_args = (struct uk_fdio_vma_args) {
		.of = mapping,
		.lifetime = &attachment->lifetime,
	};
	uk_mutex_unlock(&pprocess_mutex);
	rc = uk_posix_process_vm_map(vas, &base, length, attr, map_flags,
				     &uk_fdio_vma_ops, &mapping_args);
	uk_mutex_lock(&pprocess_mutex);
	if (!rc) {
		attachment->base = base;
		segment->status.atime = sysv_now();
		segment->status.lpid = uk_pprocess_current()->pid;
		rc = base;
	}
	dead = attachment_put_locked(attachment);
out:
	uk_mutex_unlock(&pprocess_mutex);
	if (mapping && !PTRISERR(mapping))
		uk_ofile_release(mapping);
	attachment_destroy(dead);
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, shmdt, const void *, address)
{
	struct shm_attachment *attachment, *dead = NULL;
	struct posix_vm_context *vm = uk_pprocess_current()->vm_context;
	struct uk_vas *vas = uk_vas_get_active();
	int rc = -EINVAL;

	UK_ASSERT(!uk_mutex_is_locked(&pprocess_mutex));
	uk_mutex_lock(&vas->mutation_lock);
	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry(attachment, &attachments, link) {
		if (attachment->vm != vm || attachment->retired ||
		    attachment->base != (__vaddr_t)address)
			continue;
		attachment->detaching_pid = uk_pprocess_current()->pid;
		attachment->references++;
		uk_mutex_unlock(&pprocess_mutex);
		rc = uk_fdio_unmap_lifetime(vas, &attachment->lifetime);
		uk_mutex_lock(&pprocess_mutex);
		dead = attachment_put_locked(attachment);
		break;
	}
	uk_mutex_unlock(&pprocess_mutex);
	attachment_destroy(dead);
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, shmctl, int, id, int, command, void *, buffer)
{
	struct sysv_object *object;
	struct shm_segment *segment = NULL;
	struct sysv_shmid data;
	int rc;

	command &= ~0x100;
	uk_mutex_lock(&pprocess_mutex);
	object = command == SHM_STAT || command == SHM_STAT_ANY ?
		sysv_find_index(SYSV_SHM, id) : sysv_find_id(SYSV_SHM, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	segment = (struct shm_segment *)object;
	object->references++;
	switch (command) {
	case SYSV_IPC_STAT:
	case SHM_STAT:
	case SHM_STAT_ANY:
		rc = command == SHM_STAT_ANY ? 0 : sysv_access(object, 4);
		if (rc)
			break;
		data = segment->status;
		data.perm = object->perm;
		rc = uk_nofault_try_write(data, buffer) ?
			(command == SYSV_IPC_STAT ? 0 : object->id) : -EFAULT;
		break;
	case SYSV_IPC_SET:
		if (!uk_nofault_try_read(data, buffer)) {
			rc = -EFAULT;
			break;
		}
		rc = sysv_set_perm(object, &data.perm);
		if (!rc)
			segment->status.ctime = sysv_now();
		break;
	case SYSV_IPC_RMID:
		rc = sysv_owner(object);
		if (!rc) {
			object->perm.mode |= SHM_DEST;
			object->perm.key = 0;
			if (!segment->status.nattch)
				sysv_remove(object);
		}
		break;
	case SHM_LOCK:
	case SHM_UNLOCK:
		rc = sysv_owner(object);
		if (!rc) {
			if (command == SHM_LOCK)
				object->perm.mode |= SHM_LOCKED;
			else
				object->perm.mode &= ~SHM_LOCKED;
		}
		break;
	default:
		rc = -EINVAL;
	}
out:
	uk_mutex_unlock(&pprocess_mutex);
	if (segment)
		shm_segment_put(segment);
	return rc;
}

void pprocess_shm_retire_locked(struct posix_vm_context *state, pid_t pid)
{
	struct shm_attachment *attachment;

	UK_ASSERT(uk_mutex_is_locked(&pprocess_mutex));
	uk_list_for_each_entry(attachment, &attachments, link) {
		if (attachment->vm != state || attachment->retired)
			continue;
		attachment->retired = true;
		attachment->detaching_pid = pid;
		shm_detach_count_locked(attachment, attachment->mappings, pid);
	}
}

#if CONFIG_LIBUKTEST &&                                                        \
	(CONFIG_LIBPOSIX_PROCESS_VM_LIFETIME_TEST || CONFIG_LIBUKTEST_ALL)
#include <uk/test.h>

UK_TESTCASE(posix_shm_lifetime,
	    retired_mappings_do_not_restore_logical_attachments)
{
	struct posix_vm_context vm = {0};
	struct shm_segment segment = {.status.nattch = 2};
	struct shm_attachment attachment = {
		.vm = &vm,
		.segment = &segment,
		.references = 3,
		.mappings = 2,
		.lifetime = {
			.acquire = shm_mapping_acquire,
			.release = shm_mapping_release,
		},
	};

	uk_mutex_lock(&pprocess_mutex);
	uk_list_add_tail(&attachment.link, &attachments);
	pprocess_shm_retire_locked(&vm, 123);
	UK_TEST_EXPECT(attachment.retired);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.nattch, 0);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.lpid, 123);
	UK_TEST_EXPECT_SNUM_EQ(attachment.references, 3);
	UK_TEST_EXPECT_SNUM_EQ(attachment.mappings, 2);
	pprocess_shm_retire_locked(&vm, 123);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.nattch, 0);
	uk_mutex_unlock(&pprocess_mutex);
	shm_mapping_acquire(&attachment.lifetime);
	UK_TEST_EXPECT_SNUM_EQ(attachment.mappings, 3);
	UK_TEST_EXPECT_SNUM_EQ(attachment.references, 4);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.nattch, 0);
	shm_mapping_release(&attachment.lifetime);
	shm_mapping_release(&attachment.lifetime);
	shm_mapping_release(&attachment.lifetime);
	UK_TEST_EXPECT_SNUM_EQ(attachment.mappings, 0);
	UK_TEST_EXPECT_SNUM_EQ(attachment.references, 1);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.nattch, 0);
	UK_TEST_EXPECT_SNUM_EQ(segment.status.lpid, 123);
	uk_mutex_lock(&pprocess_mutex);
	uk_list_del(&attachment.link);
	uk_mutex_unlock(&pprocess_mutex);
}

uk_testsuite_rootfs_prio(posix_shm_lifetime, NULL, UK_PRIO_LATEST);
#endif
