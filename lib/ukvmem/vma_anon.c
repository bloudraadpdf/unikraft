/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <stddef.h>
#include <errno.h>

#include <uk/config.h>
#include <uk/assert.h>
#include <uk/refcount.h>
#include <uk/vmem/vma_ops.h>
#include <uk/arch/limits.h>
#ifdef CONFIG_LIBUKPAGING
#include <uk/paging.h>
#include <uk/falloc.h>
#endif /* CONFIG_LIBUKPAGING */
#include <uk/isr/string.h>

struct uk_vma_anon_identity {
	struct uk_alloc *allocator;
	__atomic references;
	__vaddr_t start;
};

struct uk_vma_anon {
	struct uk_vma vma;
	struct uk_vma_anon_identity *identity;
};

static struct uk_vma_anon_identity *anon_identity(const struct uk_vma *vma)
{
	return __containerof(vma, struct uk_vma_anon, vma)->identity;
}

struct uk_vma_anon_identity *
uk_vma_anon_identity_acquire(const struct uk_vma *vma, __vaddr_t address,
			     __sz *offset)
{
	struct uk_vma_anon_identity *identity;

	if (!vma || vma->ops != &uk_vma_anon_ops ||
	    !(vma->flags & UK_VMA_ANON_SHARED) ||
	    address < vma->start || address >= vma->end)
		return NULL;
	identity = anon_identity(vma);
	uk_refcount_acquire(&identity->references);
	*offset = address - identity->start;
	return identity;
}

void uk_vma_anon_identity_release(struct uk_vma_anon_identity *identity)
{
	UK_ASSERT(identity);
	if (uk_refcount_release(&identity->references))
		uk_free(identity->allocator, identity);
}

static int vma_op_anon_new(struct uk_vas *vas, __vaddr_t address,
			   __sz length __unused, void *data __unused,
			   unsigned long attr __unused, unsigned long *flags,
			   struct uk_vma **output)
{
	struct uk_vma_anon *anon;
	struct uk_alloc *allocator;
	struct uk_vma *vma;

	if (!(*flags & UK_VMA_ANON_SHARED)) {
		vma = uk_malloc(vas->a, sizeof(*vma));
		if (!vma)
			return -ENOMEM;
		vma->name = NULL;
		*output = vma;
		return 0;
	}
	allocator = uk_alloc_get_default();
	if (!allocator)
		return -ENOMEM;
	anon = uk_malloc(vas->a, sizeof(*anon));
	if (!anon)
		return -ENOMEM;
	anon->identity = uk_malloc(allocator, sizeof(*anon->identity));
	if (!anon->identity) {
		uk_free(vas->a, anon);
		return -ENOMEM;
	}
	anon->identity->allocator = allocator;
	anon->identity->start = address;
	uk_refcount_init(&anon->identity->references, 1);
	anon->vma.name = NULL;
	*output = &anon->vma;
	return 0;
}

static void vma_op_anon_destroy(struct uk_vma *vma)
{
	if (vma->flags & UK_VMA_ANON_SHARED)
		uk_vma_anon_identity_release(anon_identity(vma));
}

static int vma_op_anon_split(struct uk_vma *vma, __vaddr_t address __unused,
			     struct uk_vma **output)
{
	struct uk_vma_anon *next;

	if (!(vma->flags & UK_VMA_ANON_SHARED))
		return 0;
	next = uk_malloc(vma->vas->a, sizeof(*next));
	if (!next)
		return -ENOMEM;
	next->identity = anon_identity(vma);
	uk_refcount_acquire(&next->identity->references);
	*output = &next->vma;
	return 0;
}

static int vma_op_anon_merge(struct uk_vma *vma, struct uk_vma *next)
{
	if (!(vma->flags & UK_VMA_ANON_SHARED))
		return 0;
	return anon_identity(vma) == anon_identity(next) ? 0 : -EPERM;
}

#ifdef CONFIG_LIBUKVMEM_ANON_BASE
static __vaddr_t vma_op_anon_get_base(struct uk_vas *vas __unused,
				      void *data __unused,
				      unsigned long flags __unused)
{
	return CONFIG_LIBUKVMEM_ANON_BASE;
}
#endif /* CONFIG_LIBUKVMEM_ANON_BASE */

static int vma_op_anon_fault(struct uk_vma *vma, struct uk_vm_fault *fault)
{
	struct uk_pagetable * const pt = vma->vas->pt;
	unsigned long pages = fault->len / UK_PAGING_PAGE_SIZE;
	__paddr_t paddr = UK_PAGING_PADDR_ANY;
	__vaddr_t vaddr;
	int rc;

	UK_ASSERT(UK_PAGING_PAGE_ALIGNED(fault->len));
	UK_ASSERT(fault->len == UK_PAGING_PAGE_Lx_SIZE(fault->level));
	UK_ASSERT(fault->type & UK_VMA_FAULT_NONPRESENT);

	rc = pt->fa->falloc(pt->fa, &paddr, pages, FALLOC_FLAG_ALIGNED);
	if (unlikely(rc))
		return rc;

	if (!(vma->flags & UK_VMA_FLAG_UNINITIALIZED)) {
		vaddr = uk_paging_page_kmap(pt, paddr, pages, 0);
		if (unlikely(vaddr == UK_PAGING_VADDR_INV)) {
			pt->fa->ffree(pt->fa, paddr, pages);
			return -ENOMEM;
		}

		memset_isr((void *)vaddr, 0, fault->len);
		uk_paging_page_kunmap(pt, vaddr, pages, 0);
	}

	fault->paddr = paddr;
	return 0;
}

const struct uk_vma_ops uk_vma_anon_ops = {
#ifdef CONFIG_LIBUKVMEM_ANON_BASE
	.get_base	= vma_op_anon_get_base,
#else /* CONFIG_LIBUKVMEM_ANON_BASE */
	.get_base	= __NULL,
#endif /* !CONFIG_LIBUKVMEM_ANON_BASE */
	.new		= vma_op_anon_new,
	.destroy	= vma_op_anon_destroy,
	.fault		= vma_op_anon_fault,
	.fault_discard	= uk_vma_op_fault_discard,
	.unmap		= uk_vma_op_unmap,	/* default */
	.split		= vma_op_anon_split,
	.merge		= vma_op_anon_merge,
	.set_attr	= uk_vma_op_set_attr,	/* default */
	.advise		= uk_vma_op_advise,	/* default */
};
