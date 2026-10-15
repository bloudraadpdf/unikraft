/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <errno.h>
#include <string.h>

#include <uk/config.h>
#include <uk/assert.h>
#include <uk/falloc.h>
#include <uk/posix-fdtab.h>
#include <uk/posix-file-vma.h>
#include <uk/vmem/vma_ops.h>
#include <uk/arch/limits.h>
#include <uk/paging.h>

#include "fdio-impl.h"

struct file_page {
	struct file_page *next;
	__off offset;
	__paddr_t physical;
};

struct file_vma {
	struct uk_vma base;

	/** Open file description mapped by this VMA */
	struct uk_ofile *of;
	/** Start offset describing what position in the file is mapped */
	__off offset;
	struct file_page *pages;
	int shared;
};

#ifdef CONFIG_LIBUKVMEM_FILE_BASE
static
__vaddr_t file_vma_op_get_base(struct uk_vas *vas __unused,
			       void *data __unused,
			       unsigned long flags __unused)
{
	return CONFIG_LIBUKVMEM_FILE_BASE;
}
#else /* !CONFIG_LIBUKVMEM_FILE_BASE */
#define file_vma_op_get_base __NULL
#endif /* !CONFIG_LIBUKVMEM_FILE_BASE */

static
int file_vma_op_new(struct uk_vas *vas, __vaddr_t vaddr __unused,
		    __sz len, void *data, unsigned long attr,
		    unsigned long *flags, struct uk_vma **vma)
{
	struct uk_fdio_vma_args *args = data;
	struct file_vma *fvma;
	unsigned int mode;

	UK_ASSERT(data);
	UK_ASSERT(args->of);
	UK_ASSERT(args->offset >= 0);
	UK_ASSERT(UK_PAGING_PAGE_ALIGNED(args->offset));
	UK_ASSERT(vma);

	mode = args->of->mode;
	if (!_CAN_READ(mode) || ((*flags & UK_VMA_FILE_SHARED) &&
	    (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) && !_CAN_WRITE(mode)))
		return -EACCES;
	if ((__off)len < 0 || args->offset > __OFF_MAX - (__off)len)
		return -EOVERFLOW;

	int shared = !!(*flags & UK_VMA_FILE_SHARED);
	if (shared) {
		ssize_t rc;
		uk_file_wlock(args->of->file);
		rc = uk_file_mem(args->of->file, UKFILE_MEM_ACQUIRE,
				 args->offset, 0, NULL, 0);
		uk_file_wunlock(args->of->file);
		if (rc < 0) {
			if ((attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) ||
			    (rc != -EOPNOTSUPP && rc != -ENODEV))
				return rc;
			shared = 0;
		}
	}
	fvma = uk_malloc(vas->a, sizeof(*fvma));
	if (unlikely(!fvma))
		return -ENOMEM;
	uk_ofile_acquire(args->of);
	fvma->of = args->of;
	fvma->offset = args->offset;
	fvma->pages = NULL;
	fvma->shared = shared;
	fvma->base.name = uk_ofile_name(args->of, "file");

	*vma = &fvma->base;
	return 0;
}

static void file_pages_release(struct file_vma *fvma, __u64 start)
{
	const struct uk_file *file = fvma->of->file;

	if (!fvma->pages)
		return;
	uk_file_wlock(file);
	struct file_page **entry = &fvma->pages;
	while (*entry) {
		struct file_page *page = *entry;
		if ((__u64)page->offset < start) {
			entry = &page->next;
			continue;
		}
		uk_file_mem(file, UKFILE_MEM_RELEASE, page->offset,
			    UK_PAGING_PAGE_SIZE, NULL, 0);
		*entry = page->next;
		uk_free(fvma->base.vas->a, page);
	}
	uk_file_wunlock(file);
}

static int file_shared_fault(struct file_vma *fvma, struct uk_vm_fault *fault)
{
	const struct uk_file *file = fvma->of->file;
	const __off off = fault->vbase - fvma->base.start + fvma->offset;
	struct file_page *page = NULL;
	struct uk_statx stat;
	struct iovec buffer;
	ssize_t rc;

	if (fault->len != UK_PAGING_PAGE_SIZE)
		return -ENOMEM;
	uk_file_wlock(file);
	rc = uk_file_getstat(file, UK_STATX_SIZE, &stat);
	if (rc)
		goto fail;
	if ((__u64)off >= stat.stx_size) {
		rc = -ENXIO;
		goto fail;
	}
	for (page = fvma->pages; page; page = page->next)
		if (page->offset == off) {
			fault->paddr = page->physical;
			uk_file_wunlock(file);
			return 0;
		}
	page = uk_malloc(fvma->base.vas->a, sizeof(*page));
	if (!page) {
		rc = -ENOMEM;
		goto fail;
	}
	rc = uk_file_mem(file, UKFILE_MEM_ACQUIRE, off, fault->len, NULL, 0);
	if (rc <= 0) {
		if (!rc)
			rc = -ENXIO;
		goto fail;
	}
	rc = uk_file_mem(file, UKFILE_MEM_RETRIEVE, off, fault->len, &buffer, 1);
	if (rc == -EROFS && !(fvma->base.attr & UK_PAGING_PAGE_ATTR_PROT_WRITE))
		rc = uk_file_mem(file, UKFILE_MEM_BORROW, off, fault->len, &buffer, 1);
	if (rc <= 0 || !buffer.iov_base ||
	    !UK_PAGING_PAGE_ALIGNED((__vaddr_t)buffer.iov_base)) {
		if (rc >= 0)
			rc = -EIO;
		uk_file_mem(file, UKFILE_MEM_RELEASE, off, fault->len, NULL, 0);
		goto fail;
	}
	page->offset = off;
	page->physical = uk_paging_virt_to_phys((__vaddr_t)buffer.iov_base);
	page->next = fvma->pages;
	fvma->pages = page;
	fault->paddr = page->physical;
	uk_file_wunlock(file);
	return 0;
fail:
	uk_file_wunlock(file);
	uk_free(fvma->base.vas->a, page);
	return rc;
}

static
void file_vma_op_destroy(struct uk_vma *vma)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	UK_ASSERT(fvma->of);
	file_pages_release(fvma, 0);
	uk_ofile_release(fvma->of);
}

static inline
__ssz file_vma_readin(const struct uk_file *f, __vaddr_t buf, __sz len,
		      __off offset)
{
	__ssz read = 0;
	__ssz r;
	struct uk_statx stat;

	uk_file_rlock(f);
	r = uk_file_getstat(f, UK_STATX_SIZE, &stat);
	if (r)
		goto out;
	if ((__u64)offset >= stat.stx_size) {
		r = -ENXIO;
		goto out;
	}
	/* Read up until done, error, or confirmed EOF */
	do {
		r = uk_file_read(f, &(struct iovec){ (void *)buf, len }, 1,
				 offset, 0);
		if (unlikely(r <= 0))
			break;
		buf += r;
		len -= r;
		offset += r;
		read += r;
	} while (len);
out:
	uk_file_runlock(f);
	/* Only return success if done reading or EOF */
	if (!len || !r)
		return read;
	/* We should only get here on I/O error */
	UK_ASSERT(r < 0);
	return r;
}

static
int file_vma_op_fault(struct uk_vma *vma, struct uk_vm_fault *fault)
{
	struct file_vma *fvma = (struct file_vma *)vma;
	struct uk_pagetable *const pt = vma->vas->pt;
	unsigned long pages = fault->len / UK_PAGING_PAGE_SIZE;
	__paddr_t paddr = UK_PAGING_PADDR_ANY;
	__vaddr_t vaddr;
	__off off;
	__ssz nbytes;
	int rc;

	UK_ASSERT(UK_PAGING_PAGE_ALIGNED(fault->len));
	UK_ASSERT(fault->len == UK_PAGING_PAGE_Lx_SIZE(fault->level));
	UK_ASSERT(fault->type & UK_VMA_FAULT_NONPRESENT);

	if (fvma->shared)
		return file_shared_fault(fvma, fault);

	rc = pt->fa->falloc(pt->fa, &paddr, pages, FALLOC_FLAG_ALIGNED);
	if (unlikely(rc))
		return rc;

	vaddr = uk_paging_page_kmap(pt, paddr, pages, 0);
	if (unlikely(vaddr == UK_PAGING_VADDR_INV)) {
		pt->fa->ffree(pt->fa, paddr, pages);
		return -ENOMEM;
	}

	off = (fault->vbase - vma->start) + fvma->offset;

	nbytes = file_vma_readin(fvma->of->file, vaddr, fault->len, off);
	if (unlikely(nbytes < 0)) {
		uk_paging_page_kunmap(pt, vaddr, pages, 0);
		pt->fa->ffree(pt->fa, paddr, pages);
		return nbytes;
	}

	/* Fill the remaining space with zeros */
	UK_ASSERT(fault->len >= (__sz)nbytes);

	memset((void *)(vaddr + nbytes), 0, fault->len - nbytes);
	uk_paging_page_kunmap(pt, vaddr, pages, 0);

	fault->paddr = paddr;
	return 0;
}

static
int file_vma_op_split(struct uk_vma *vma, __vaddr_t vaddr,
		      struct uk_vma **new_vma)
{
	struct file_vma *fvma = (struct file_vma *)vma;
	struct file_vma *v;
	__off off = vaddr - vma->start;

	UK_ASSERT(vaddr >= vma->start);
	UK_ASSERT(fvma->offset <= __OFF_MAX - off);
	UK_ASSERT(new_vma);

	v = uk_malloc(vma->vas->a, sizeof(*v));
	if (unlikely(!v))
		return -ENOMEM;

	v->offset = fvma->offset + off;
	uk_ofile_acquire(fvma->of);
	v->of = fvma->of;
	v->shared = fvma->shared;
	v->pages = NULL;
	struct file_page **entry = &fvma->pages;
	while (*entry) {
		struct file_page *page = *entry;
		if (page->offset < v->offset) {
			entry = &page->next;
			continue;
		}
		*entry = page->next;
		page->next = v->pages;
		v->pages = page;
	}

	*new_vma = &v->base;
	return 0;
}

static
int file_vma_op_merge(struct uk_vma *vma, struct uk_vma *next)
{
	struct file_vma *fvma = (struct file_vma *)vma;
	struct file_vma *nvma = (struct file_vma *)next;
	__off off;

	UK_ASSERT(next->start == vma->end);
	UK_ASSERT(next->start > vma->start);

	/* Only merge if this is the same open file description ... */
	if (fvma->of != nvma->of || fvma->shared != nvma->shared)
		return -EPERM;
	/* ... and the VMAs map contiguous file ranges */
	off = next->start - vma->start;
	if (nvma->offset != fvma->offset + off)
		return -EPERM;
	while (nvma->pages) {
		struct file_page *page = nvma->pages;
		nvma->pages = page->next;
		page->next = fvma->pages;
		fvma->pages = page;
	}
	/* We release the file in the destructor */
	return 0;
}

static
int file_vma_op_set_attr(struct uk_vma *vma, unsigned long attr)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	if ((vma->flags & UK_VMA_FILE_SHARED) &&
	    (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) &&
	    (!fvma->shared || !_CAN_WRITE(fvma->of->mode)))
		return -EACCES;
	if ((attr & UK_PAGING_PAGE_ATTR_PROT_READ) &&
	    !_CAN_READ(fvma->of->mode))
		return -EACCES;
	/* Default handler */
	return uk_vma_op_set_attr(vma, attr);
}

static void file_vma_fault_discard(struct uk_vma *vma, struct uk_vm_fault *fault)
{
	if (!((struct file_vma *)vma)->shared)
		uk_vma_op_fault_discard(vma, fault);
}

static int file_vma_unmap(struct uk_vma *vma, __vaddr_t addr, __sz len)
{
	if (!((struct file_vma *)vma)->shared)
		return uk_vma_op_unmap(vma, addr, len);
	return uk_paging_page_unmap(vma->vas->pt, addr, len / UK_PAGING_PAGE_SIZE,
				    UK_PAGING_PAGE_FLAG_KEEP_FRAMES);
}

static int file_vma_advise(struct uk_vma *vma, __vaddr_t addr, __sz len,
		unsigned long advice)
{
	if (((struct file_vma *)vma)->shared &&
	    (advice & UK_VMA_ADV_DONTNEED) && !(advice & UK_VMA_ADV_WILLNEED))
		return file_vma_unmap(vma, addr, len);
	return uk_vma_op_advise(vma, addr, len, advice);
}

static int file_vma_sync(struct uk_vma *vma, __vaddr_t addr __unused,
		__sz len __unused)
{
	struct file_vma *fvma = (struct file_vma *)vma;
	int rc;

	if (!fvma->shared)
		return 0;
	uk_file_wlock(fvma->of->file);
	rc = uk_file_ctl(fvma->of->file, UKFILE_CTL_FILE, UKFILE_CTL_FILE_SYNC,
			  1, 0, 0);
	uk_file_wunlock(fvma->of->file);
	return rc;
}

const struct uk_vma_ops uk_fdio_vma_ops = {
	.get_base	= file_vma_op_get_base,
	.new		= file_vma_op_new,
	.destroy	= file_vma_op_destroy,
	.fault		= file_vma_op_fault,
	.fault_can_block = 1,
	.fault_discard	= file_vma_fault_discard,
	.unmap		= file_vma_unmap,
	.split		= file_vma_op_split,
	.merge		= file_vma_op_merge,
	.set_attr	= file_vma_op_set_attr,
	.advise		= file_vma_advise,
	.sync		= file_vma_sync,
};

int uk_fdio_file_truncate(const struct uk_file *file, __off length, int iolock)
{
	struct uk_vas *vas = uk_vas_get_active();
	const struct uk_vma *vma;
	const __u64 end = UK_PAGING_PAGE_ALIGN_UP((__u64)length);
	int rc;

	uk_mutex_lock(&vas->mutation_lock);
	if (iolock)
		uk_file_wlock(file);
	rc = uk_file_ctl(file, UKFILE_CTL_FILE, UKFILE_CTL_FILE_TRUNC,
			 length, 0, 0);
	if (iolock)
		uk_file_wunlock(file);
	if (rc)
		goto out;
	for (vma = uk_vma_first(vas); vma; vma = uk_vma_next(vma)) {
		if (vma->ops != &uk_fdio_vma_ops)
			continue;
		struct file_vma *fvma = (struct file_vma *)vma;
		const struct uk_file *mapped = fvma->of->file;
		if (mapped != file && (!file->state || mapped->state != file->state))
			continue;
		__u64 first = MAX((__u64)fvma->offset, end);
		__u64 last = fvma->offset + vma->end - vma->start;
		if (first >= last)
			continue;
		rc = uk_vma_advise(vas, vma->start + first - fvma->offset,
			last - first, UK_VMA_ADV_DONTNEED, UK_VMA_FLAG_STRICT_VMA_CHECK);
		if (rc)
			break;
		file_pages_release(fvma, first);
	}
out:
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
}
