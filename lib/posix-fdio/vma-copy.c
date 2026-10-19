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
#include <uk/tree.h>

#include "fdio-impl.h"

struct file_page {
	struct file_page *next;
	UK_RB_ENTRY(file_page) index;
	__off offset;
	__paddr_t physical;
	bool written;
};

UK_RB_HEAD(file_page_index, file_page);

static __off file_page_key(struct file_page *page)
{
	return page->offset;
}

static int file_page_compare(__off first, __off second)
{
	return (first > second) - (first < second);
}

UK_RB_KEY_GENERATE_STATIC(file_page_index, file_page, index,
			  file_page_compare, file_page_key);

struct file_vma {
	struct uk_vma base;

	/** Open file description mapped by this VMA */
	struct uk_ofile *of;
	/** Start offset describing what position in the file is mapped */
	__off offset;
	struct file_page *pages;
	struct file_page_index page_index;
	int shared;
	struct uk_fdio_vma_lifetime *lifetime;
};

static void file_page_attach(struct file_vma *vma, struct file_page *page)
{
	struct file_page *old __maybe_unused;

	old = UK_RB_INSERT(file_page_index, &vma->page_index, page);
	UK_ASSERT(!old);
	page->next = vma->pages;
	vma->pages = page;
}

static void file_page_unindex(struct file_vma *vma, struct file_page *page)
{
	UK_RB_REMOVE(file_page_index, &vma->page_index, page);
}

struct uk_ofile *uk_fdio_vma_file(const struct uk_vma *vma, __off *offset)
{
	const struct file_vma *file = (const struct file_vma *)vma;

	if (vma->ops != &uk_fdio_vma_ops)
		return NULL;
	*offset = file->offset;
	uk_ofile_acquire(file->of);
	return file->of;
}

bool uk_fdio_vma_page_written(const struct uk_vma *vma, __vaddr_t address)
{
	const struct file_vma *file;
	const struct file_page *page;
	__off offset;

	if (!vma || vma->ops != &uk_fdio_vma_ops ||
	    address < vma->start || address >= vma->end)
		return false;
	file = (const struct file_vma *)vma;
	if (file->shared)
		return false;
	offset = file->offset + UK_PAGING_PAGE_ALIGN_DOWN(address - vma->start);
	for (page = file->pages; page; page = page->next)
		if (page->offset == offset)
			return page->written;
	return false;
}

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
	    (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) && !fdio_can_write(mode)))
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
	UK_RB_INIT(&fvma->page_index);
	fvma->shared = shared;
	fvma->base.name = uk_ofile_name(args->of, "file");
	fvma->lifetime = args->lifetime;
	if (fvma->lifetime)
		fvma->lifetime->acquire(fvma->lifetime);

	*vma = &fvma->base;
	return 0;
}

static void file_pages_release(struct file_vma *fvma, __u64 start, __u64 end)
{
	const struct uk_file *file = fvma->of->file;

	if (!fvma->pages)
		return;
	if (fvma->shared)
		uk_file_wlock(file);
	struct file_page **entry = &fvma->pages;
	while (*entry) {
		struct file_page *page = *entry;
		if ((__u64)page->offset < start || (__u64)page->offset >= end) {
			entry = &page->next;
			continue;
		}
		file_page_unindex(fvma, page);
		if (fvma->shared)
			uk_file_mem(file, UKFILE_MEM_RELEASE, page->offset,
				    UK_PAGING_PAGE_SIZE, NULL, 0);
		*entry = page->next;
		uk_free(fvma->base.vas->a, page);
	}
	if (fvma->shared)
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
	page = UK_RB_FIND(file_page_index, &fvma->page_index, off);
	if (page) {
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
	file_page_attach(fvma, page);
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
	file_pages_release(fvma, 0, __U64_MAX);
	uk_ofile_release(fvma->of);
	if (fvma->lifetime)
		fvma->lifetime->release(fvma->lifetime);
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
	struct file_page *created = NULL, *page;
	bool written = (fault->type & UK_VMA_FAULT_ACCESSTYPE) ==
		UK_VMA_FAULT_WRITE;
	int rc;

	UK_ASSERT(UK_PAGING_PAGE_ALIGNED(fault->len));
	UK_ASSERT(fault->len == UK_PAGING_PAGE_Lx_SIZE(fault->level));
	if (fvma->shared) {
		if (!(fault->type & UK_VMA_FAULT_NONPRESENT))
			return -EFAULT;
		return file_shared_fault(fvma, fault);
	}
	off = (fault->vbase - vma->start) + fvma->offset;
	if (!(fault->type & UK_VMA_FAULT_NONPRESENT)) {
		if (!written)
			return -EFAULT;
		for (__sz index = 0; index < fault->len;
		     index += UK_PAGING_PAGE_SIZE) {
			page = UK_RB_FIND(file_page_index, &fvma->page_index,
					  off + (__off)index);
			if (!page)
				return -EFAULT;
		}
		for (__sz index = 0; index < fault->len;
		     index += UK_PAGING_PAGE_SIZE) {
			page = UK_RB_FIND(file_page_index, &fvma->page_index,
					  off + (__off)index);
			page->written = true;
		}
		return 0;
	}
	for (__sz index = 0; index < fault->len;
	     index += UK_PAGING_PAGE_SIZE) {
		page = uk_malloc(vma->vas->a, sizeof(*page));
		if (!page) {
			rc = -ENOMEM;
			goto fail;
		}
		*page = (struct file_page){
			.next = created, .offset = off + index,
			.written = written
		};
		created = page;
	}

	rc = pt->fa->falloc(pt->fa, &paddr, pages, FALLOC_FLAG_ALIGNED);
	if (unlikely(rc))
		goto fail;

	vaddr = uk_paging_page_kmap(pt, paddr, pages, 0);
	if (unlikely(vaddr == UK_PAGING_VADDR_INV)) {
		pt->fa->ffree(pt->fa, paddr, pages);
		rc = -ENOMEM;
		goto fail;
	}

	nbytes = file_vma_readin(fvma->of->file, vaddr, fault->len, off);
	if (unlikely(nbytes < 0)) {
		uk_paging_page_kunmap(pt, vaddr, pages, 0);
		pt->fa->ffree(pt->fa, paddr, pages);
		rc = nbytes;
		goto fail;
	}

	/* Fill the remaining space with zeros */
	UK_ASSERT(fault->len >= (__sz)nbytes);

	memset((void *)(vaddr + nbytes), 0, fault->len - nbytes);
	uk_paging_page_kunmap(pt, vaddr, pages, 0);

	fault->paddr = paddr;
	if (!written) {
		unsigned long attr = vma->attr;

		attr &= ~UK_PAGING_PAGE_ATTR_PROT_WRITE;

		fault->pte = uk_pal_pte_change_attr(fault->pte, attr,
						    fault->level);
	}
	while (created) {
		page = created;
		created = page->next;
		page->physical = paddr + page->offset - off;
		file_page_attach(fvma, page);
	}
	return 0;
fail:
	while (created) {
		page = created;
		created = page->next;
		uk_free(vma->vas->a, page);
	}
	return rc;
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
	UK_RB_INIT(&v->page_index);
	v->lifetime = fvma->lifetime;
	if (v->lifetime)
		v->lifetime->acquire(v->lifetime);
	struct file_page **entry = &fvma->pages;
	while (*entry) {
		struct file_page *page = *entry;
		if (page->offset < v->offset) {
			entry = &page->next;
			continue;
		}
		*entry = page->next;
		file_page_unindex(fvma, page);
		file_page_attach(v, page);
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
	if (fvma->of != nvma->of || fvma->shared != nvma->shared ||
	    fvma->lifetime != nvma->lifetime)
		return -EPERM;
	/* ... and the VMAs map contiguous file ranges */
	off = next->start - vma->start;
	if (nvma->offset != fvma->offset + off)
		return -EPERM;
	while (nvma->pages) {
		struct file_page *page = nvma->pages;
		nvma->pages = page->next;
		file_page_unindex(nvma, page);
		file_page_attach(fvma, page);
	}
	/* We release the file in the destructor */
	return 0;
}

int uk_fdio_unmap_lifetime(struct uk_vas *vas,
			   struct uk_fdio_vma_lifetime *lifetime)
{
	struct uk_vma *vma;
	int rc = 0;

	uk_mutex_lock(&vas->mutation_lock);
	for (;;) {
		uk_list_for_each_entry(vma, &vas->vma_list, vma_list) {
			struct file_vma *file = (struct file_vma *)vma;

			if (vma->ops == &uk_fdio_vma_ops &&
			    file->lifetime == lifetime)
				goto unmap;
		}
		break;
unmap:
		rc = uk_vma_unmap(vas, vma->start, vma->end - vma->start, 0);
		if (rc)
			break;
	}
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
}

static
int file_vma_op_set_attr(struct uk_vma *vma, unsigned long attr)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	if ((vma->flags & UK_VMA_FILE_SHARED) &&
	    (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE) &&
	    (!fvma->shared || !fdio_can_write(fvma->of->mode)))
		return -EACCES;
	if ((attr & UK_PAGING_PAGE_ATTR_PROT_READ) &&
	    !_CAN_READ(fvma->of->mode))
		return -EACCES;
	if (fvma->shared)
		return uk_vma_op_set_attr(vma, attr);
	int rc = uk_vma_op_set_attr(vma,
			attr & ~UK_PAGING_PAGE_ATTR_PROT_WRITE);
	if (rc || !(attr & UK_PAGING_PAGE_ATTR_PROT_WRITE))
		return rc;
	for (struct file_page *page = fvma->pages; page; page = page->next) {
		if (!page->written)
			continue;
		__vaddr_t address = vma->start + page->offset - fvma->offset;

		rc = uk_paging_page_set_attr(vma->vas->pt, address, 1, attr, 0);
		if (rc)
			return rc;
	}
	return 0;
}

static void file_vma_fault_discard(struct uk_vma *vma, struct uk_vm_fault *fault)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	if (!fvma->shared) {
		struct file_page **entry = &fvma->pages;

		while (*entry) {
			struct file_page *page = *entry;

			if (page->physical < fault->paddr ||
			    page->physical - fault->paddr >= fault->len) {
				entry = &page->next;
				continue;
			}
			*entry = page->next;
			file_page_unindex(fvma, page);
			uk_free(vma->vas->a, page);
		}
		uk_vma_op_fault_discard(vma, fault);
	}
}

static void file_vma_unmap_release(struct uk_vma *vma, __vaddr_t addr,
				   __sz len)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	if (!fvma->shared) {
		__u64 offset = fvma->offset + addr - vma->start;

		file_pages_release(fvma, offset, offset + len);
	}
}

static int file_vma_unmap(struct uk_vma *vma, __vaddr_t addr, __sz len)
{
	struct file_vma *fvma = (struct file_vma *)vma;

	if (!fvma->shared)
		return uk_vma_op_unmap(vma, addr, len);
	return uk_paging_page_unmap(vma->vas->pt, addr, len / UK_PAGING_PAGE_SIZE,
				    UK_PAGING_PAGE_FLAG_KEEP_FRAMES);
}

static int file_vma_advise(struct uk_vma *vma, __vaddr_t addr, __sz len,
		unsigned long advice)
{
	if ((advice & UK_VMA_ADV_DONTNEED) && !(advice & UK_VMA_ADV_WILLNEED))
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
	.unmap_release	= file_vma_unmap_release,
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
		file_pages_release(fvma, first, __U64_MAX);
	}
out:
	uk_mutex_unlock(&vas->mutation_lock);
	return rc;
}
