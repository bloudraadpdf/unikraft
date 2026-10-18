/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/futex.h>
#include <uk/nofault.h>
#include <uk/thread.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>
#endif
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
#include <uk/posix-file-vma.h>
#endif

static int key_fault_result(int rc)
{
	return !rc || rc == -ENOMEM ? rc : -EFAULT;
}

#if CONFIG_LIBUKVMEM
static bool key_mapping_owned(const struct uk_vma *vma,
			      const struct uk_futex_key *key)
{
	if (!vma || !vma->owner)
		return true;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	return uk_posix_futex_mapping_owned(vma, key->vm);
#else
	(void)key;
	return false;
#endif
}
#endif

int uk_futex_key_fault(__u32 *address, bool write,
		       const struct uk_futex_key *key)
{
	int rc;
#if CONFIG_LIBUKVMEM
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	unsigned int access = write ? UK_VMA_FAULT_WRITE : UK_VMA_FAULT_READ;

	uk_mutex_lock(&vas->mutation_lock);
	vma = uk_vma_find(vas, (__uptr)address);
	rc = key_mapping_owned(vma, key) ?
		uk_vma_populate_page(vas, (__uptr)address, access) : -EFAULT;
	uk_mutex_unlock(&vas->mutation_lock);
#else
	__u32 value, expected;

	(void)key;
	do {
		rc = uk_nofault_read32(address, &value, 0);
		if (rc || !write)
			break;
		expected = value;
		rc = uk_nofault_cmpxchg32(address, &value, expected, 0);
	} while (!rc && value != expected);
#endif
	return key_fault_result(rc);
}

int uk_futex_key_mapping(__u32 *address, bool write, struct uk_futex_key *key)
{
#if CONFIG_LIBUKVMEM
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	__vaddr_t location = (__uptr)address;
	__sz offset;
	unsigned int access = UK_VMA_FAULT_WRITE;
	int rc = -EFAULT;
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
	__off file_offset;
	bool file;
#else
	(void)write;
#endif

	uk_mutex_lock(&vas->mutation_lock);
	vma = uk_vma_find(vas, location);
	if (!key_mapping_owned(vma, key))
		goto out;
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
	file = vma && vma->ops == &uk_fdio_vma_ops;
	if (file && !write &&
	    !(vma->attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) &&
	    !uk_fdio_vma_page_written(vma, location))
		access = UK_VMA_FAULT_READ;
#endif
	rc = key_fault_result(uk_vma_populate_page(vas, location, access));
	if (rc)
		goto out;
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
	if (file && (!(vma->flags & UK_VMA_FLAG_PRIVATE) ||
		     !uk_fdio_vma_page_written(vma, location)))
		key->file = uk_fdio_vma_file(vma, &file_offset);
	if (key->file) {
		key->kind = UK_FUTEX_KEY_FILE;
		key->identity = key->file->file->state;
		key->offset = file_offset + location - vma->start;
		goto out;
	}
#endif
	key->anon = uk_vma_anon_identity_acquire(vma, location, &offset);
	if (key->anon) {
		key->kind = UK_FUTEX_KEY_ANON;
		key->identity = key->anon;
		key->offset = offset;
	}
out:
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
#else
	(void)write;
	return uk_futex_key_fault(address, true, key);
#endif
}

int uk_futex_key_get(struct uk_thread *thread, __u32 *address,
		     bool private_key, bool write, struct uk_futex_key *key)
{
	int rc;

	*key = (struct uk_futex_key){
		.kind = private_key ? UK_FUTEX_KEY_PRIVATE : UK_FUTEX_KEY_VM,
		.offset = (__uptr)address,
	};
	if ((__uptr)address % sizeof(*address))
		return -EINVAL;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	key->vm = uk_posix_futex_vm_get(thread);
	key->identity = key->vm;
#else
	(void)thread;
#endif
	if (private_key)
		return 0;
	rc = uk_futex_key_mapping(address, write, key);
	if (rc)
		uk_futex_key_put(key);
	return rc;
}

void uk_futex_key_put(struct uk_futex_key *key)
{
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	if (key->vm)
		uk_posix_vm_reference_put(key->vm);
#endif
#if CONFIG_LIBPOSIX_FDIO_VMA_COPY
	if (key->file)
		uk_ofile_release(key->file);
#endif
#if CONFIG_LIBUKVMEM
	if (key->anon)
		uk_vma_anon_identity_release(key->anon);
#endif
	*key = (struct uk_futex_key){0};
}

int uk_futex_key_recheck(__u32 *address, bool write,
			 const struct uk_futex_key *key)
{
	struct uk_futex_key query = {
		.kind = UK_FUTEX_KEY_VM, .identity = key->vm,
		.offset = (__uptr)address, .vm = key->vm,
	};
	int rc;

	if (key->kind == UK_FUTEX_KEY_PRIVATE)
		return uk_futex_key_fault(address, write, key);
	rc = uk_futex_key_mapping(address, write, &query);
	if (!rc && !uk_futex_key_equal(key, &query))
		rc = -EFAULT;
	/* The pinned key owns the borrowed VM reference. */
	query.vm = NULL;
	uk_futex_key_put(&query);
	return rc;
}
