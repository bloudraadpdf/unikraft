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
	pid_t detaching_pid;
};

static UK_LIST_HEAD(attachments);

static void shm_destroy(struct sysv_object *object)
{
	struct shm_segment *segment = (struct shm_segment *)object;

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

UK_LLSYSCALL_R_DEFINE(int, shmget, int, key, size_t, size, int, flags)
{
	struct sysv_object *object;
	struct shm_segment *segment;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_key(SYSV_SHM, key, flags);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	if (object) {
		segment = (struct shm_segment *)object;
		rc = size > segment->status.size ? -EINVAL : object->id;
		goto out;
	}
	if (!size || size > LONG_MAX - UK_PAGING_PAGE_SIZE) {
		rc = -EINVAL;
		goto out;
	}
	if (flags & 04000) {
		rc = -EOPNOTSUPP;
		goto out;
	}
	segment = uk_zalloc(uk_alloc_get_default(), sizeof(*segment));
	if (!segment) {
		rc = -ENOMEM;
		goto out;
	}
	segment->file = shm_file(size);
	if (PTRISERR(segment->file)) {
		rc = PTR2ERR(segment->file);
		uk_free(uk_alloc_get_default(), segment);
		goto out;
	}
	segment->object.destroy = shm_destroy;
	segment->status.size = size;
	segment->status.ctime = sysv_now();
	segment->status.cpid = uk_pprocess_current()->pid;
	rc = sysv_insert(&segment->object, SYSV_SHM, key, flags);
	if (rc < 0)
		shm_destroy(&segment->object);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static void attachment_put(struct shm_attachment *attachment)
{
	UK_ASSERT(attachment->references);
	if (!--attachment->references) {
		uk_list_del(&attachment->link);
		sysv_put(&attachment->segment->object);
		uk_free(uk_alloc_get_default(), attachment);
	}
}

static void shm_mapping_acquire(struct uk_fdio_vma_lifetime *lifetime)
{
	struct shm_attachment *attachment = __containerof(lifetime,
					struct shm_attachment, lifetime);

	uk_mutex_lock(&pprocess_mutex);
	attachment->references++;
	attachment->segment->status.nattch++;
	uk_mutex_unlock(&pprocess_mutex);
}

static void shm_mapping_release(struct uk_fdio_vma_lifetime *lifetime)
{
	struct shm_attachment *attachment = __containerof(lifetime,
					struct shm_attachment, lifetime);
	struct shm_segment *segment = attachment->segment;

	uk_mutex_lock(&pprocess_mutex);
	UK_ASSERT(segment->status.nattch);
	segment->status.nattch--;
	segment->status.dtime = sysv_now();
	segment->status.lpid = attachment->detaching_pid ?
		attachment->detaching_pid : uk_pprocess_current()->pid;
	if (!segment->status.nattch && !segment->object.removed &&
	    (segment->object.perm.mode & SHM_DEST))
		sysv_remove(&segment->object);
	attachment_put(attachment);
	uk_mutex_unlock(&pprocess_mutex);
}

UK_LLSYSCALL_R_DEFINE(long, shmat, int, id, const void *, address, int, flags)
{
	struct sysv_object *object;
	struct shm_segment *segment;
	struct shm_attachment *attachment;
	struct uk_ofile *mapping;
	__vaddr_t base = address ? (__vaddr_t)address : UK_PAGING_VADDR_ANY;
	unsigned long attr = UK_PAGING_PAGE_ATTR_PROT_READ;
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
		uk_ofile_release(mapping);
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
	object->references++;
	uk_list_add_tail(&attachment->link, &attachments);
	length = ALIGN_UP(segment->status.size, UK_PAGING_PAGE_SIZE);
	rc = uk_fdio_map_file(uk_vas_get_active(), &base, length, attr,
			      map_flags, mapping, 0, &attachment->lifetime);
	uk_ofile_release(mapping);
	if (!rc) {
		attachment->base = base;
		segment->status.atime = sysv_now();
		segment->status.lpid = uk_pprocess_current()->pid;
		rc = base;
	}
	attachment_put(attachment);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, shmdt, const void *, address)
{
	struct shm_attachment *attachment;
	struct posix_vm_context *vm = uk_pprocess_current()->vm_context;
	int rc = -EINVAL;

	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry(attachment, &attachments, link) {
		if (attachment->vm != vm ||
		    attachment->base != (__vaddr_t)address)
			continue;
		attachment->references++;
		rc = uk_fdio_unmap_lifetime(uk_vas_get_active(),
					    &attachment->lifetime);
		attachment_put(attachment);
		break;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, shmctl, int, id, int, command, void *, buffer)
{
	struct sysv_object *object;
	struct shm_segment *segment;
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
	return rc;
}

void pprocess_shm_release(struct posix_vm_context *state, pid_t pid)
{
	struct shm_attachment *attachment, *next;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	uk_list_for_each_entry_safe(attachment, next, &attachments, link) {
		if (attachment->vm != state)
			continue;
		attachment->detaching_pid = pid;
		attachment->references++;
		rc = uk_fdio_unmap_lifetime(uk_vas_get_active(),
					    &attachment->lifetime);
		UK_ASSERT(!rc);
		attachment_put(attachment);
	}
	uk_mutex_unlock(&pprocess_mutex);
}
