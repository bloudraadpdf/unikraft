/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#include <stddef.h>
#include <errno.h>

#include "vmem.h"

#include <uk/essentials.h>
#include <uk/arch/limits.h>
#ifdef CONFIG_LIBUKPAGING
#include <uk/paging.h>
#endif /* CONFIG_LIBUKPAGING */
#include <uk/alloc.h>
#include <uk/falloc.h>
#include <uk/assert.h>
#include <uk/list.h>
#include <uk/config.h>
#include <uk/pal/except.h>

static struct uk_vas *vmem_active_vas;

/* Forward declarations */
static void vmem_vma_unmap(struct uk_vma *vma, __vaddr_t vaddr, __sz len);
static void vmem_vma_unlink_and_free(struct uk_vma *vma);

struct uk_vas *uk_vas_get_active(void)
{
	return vmem_active_vas;
}

int uk_vas_set_active(struct uk_vas *vas)
{
#ifdef CONFIG_LIBUKPAGING
	int rc;

	rc = uk_paging_pt_set_active(vas->pt);
	if (unlikely(rc))
		return rc;
#endif /* CONFIG_LIBUKPAGING */

	vmem_active_vas = vas;
	return 0;
}

int uk_vas_init(struct uk_vas *vas, struct uk_pagetable *pt __maybe_unused,
		struct uk_alloc *a)
{
	UK_ASSERT(vas);
	UK_ASSERT(pt);
	UK_ASSERT(a);

	vas->a = a;
	uk_spin_init(&vas->fault_lock);
	vas->mutation_irq_flags = 0;
	/* File reads prepare their I/O buffers through uk_vma_advise. */
	uk_mutex_init_config(&vas->mutation_lock, UK_MUTEX_CONFIG_RECURSE);

#ifdef CONFIG_LIBUKPAGING
	vas->pt = pt;

	vas->vma_base = UK_PAGING_PAGE_ALIGN_UP(CONFIG_LIBUKVMEM_DEFAULT_BASE);
	UK_ASSERT(uk_paging_vaddr_isvalid(vas->vma_base));
#endif /* CONFIG_LIBUKPAGING */

	vas->flags = 0;

	UK_INIT_LIST_HEAD(&vas->vma_list);

	return 0;
}

static void vmem_fault_lock(struct uk_vas *vas)
{
	while (!uk_spin_trylock(&vas->fault_lock)) {
		uk_paging_tlb_service();
		uk_arch_spinwait();
	}
}

static unsigned long vmem_mutation_begin(struct uk_vas *vas)
{
	unsigned long previous;

	uk_mutex_lock(&vas->mutation_lock);
	previous = vas->mutation_irq_flags;
	vas->mutation_irq_flags = uk_pal_save_irqf();
	vmem_fault_lock(vas);
	return previous;
}

static void vmem_mutation_end(struct uk_vas *vas, unsigned long previous)
{
	unsigned long flags = vas->mutation_irq_flags;

	vas->mutation_irq_flags = previous;
	uk_spin_unlock_irqrestore(&vas->fault_lock, flags);
	uk_mutex_unlock(&vas->mutation_lock);
}

static void vmem_mutation_pause(struct uk_vas *vas)
{
	uk_spin_unlock_irqrestore(&vas->fault_lock, vas->mutation_irq_flags);
}

static void vmem_mutation_resume(struct uk_vas *vas)
{
	uk_pal_disable_irq();
	vmem_fault_lock(vas);
}

static void vmem_destroy(struct uk_vas *vas)
{
	struct uk_vma *vma, *next;

	uk_list_for_each_entry_safe(vma, next, &vas->vma_list, vma_list) {
		vmem_vma_unmap(vma, vma->start, uk_vma_len(vma));
		vmem_vma_unlink_and_free(vma);
	}

	UK_ASSERT(uk_list_empty(&vas->vma_list));

	if (vmem_active_vas == vas)
		vmem_active_vas = __NULL;
}

void uk_vas_destroy(struct uk_vas *vas)
{
	unsigned long previous;

	previous = vmem_mutation_begin(vas);
	vmem_destroy(vas);
	vmem_mutation_end(vas, previous);
}

static void vmem_vma_destroy(struct uk_vma *vma)
{
	struct uk_vas *vas = vma->vas;

	UK_ASSERT(vma);

	vmem_mutation_pause(vas);
	if (vma->ops->destroy)
		vma->ops->destroy(vma);

	uk_free(vas->a, vma);
	vmem_mutation_resume(vas);
}

static void vmem_vma_unlink_and_free(struct uk_vma *vma)
{
	UK_ASSERT(vma);
	UK_ASSERT(!uk_list_empty(&vma->vma_list));

	uk_list_del(&vma->vma_list);
	vmem_vma_destroy(vma);
}

static struct uk_vma *vmem_vma_find(struct uk_vas *vas, __vaddr_t vaddr,
				    __sz len)
{
	struct uk_vma *vma;
	__vaddr_t vstart = vaddr;
	__vaddr_t vend = vaddr + MAX(len, (__sz)1);

	UK_ASSERT(vas);
	UK_ASSERT(vaddr <= __VADDR_MAX - len);

	uk_list_for_each_entry(vma, &vas->vma_list, vma_list) {
		if ((vend > vma->start) && (vstart < vma->end))
			return vma;
	}

	return __NULL;
}

const struct uk_vma *uk_vma_find(struct uk_vas *vas, __vaddr_t vaddr)
{
	const struct uk_vma *vma;

	uk_mutex_lock(&vas->mutation_lock);
	vma = vmem_vma_find(vas, vaddr, 0);
	uk_mutex_unlock(&vas->mutation_lock);
	return vma;
}

int uk_vma_stack_resize(struct uk_vas *vas, __vaddr_t address,
			__vaddr_t bottom)
{
	struct uk_vma *vma;
	__vaddr_t start;
	unsigned long previous;
	int rc = -EINVAL;

	if (!UK_PAGING_PAGE_ALIGNED(bottom) ||
	    bottom < UK_VMA_STACK_BOTTOM_GUARD_SIZE)
		return -EINVAL;
	start = bottom - UK_VMA_STACK_BOTTOM_GUARD_SIZE;
	previous = vmem_mutation_begin(vas);
	vma = vmem_vma_find(vas, address, 0);
	if (!vma || vma->ops != &uk_vma_stack_ops ||
	    bottom >= vma->end - UK_VMA_STACK_TOP_GUARD_SIZE ||
	    !uk_paging_vaddr_range_isvalid(start, vma->end - start))
		goto out;
	if (start < vma->start) {
		if (vmem_vma_find(vas, start, vma->start - start)) {
			rc = -EEXIST;
			goto out;
		}
	} else if (start > vma->start) {
		rc = vma->ops->unmap(vma, vma->start, bottom - vma->start);
		if (rc)
			goto out;
	}
	vma->start = start;
	vma->flags |= UK_VMA_STACK_GROWABLE;
	rc = 0;
out:
	vmem_mutation_end(vas, previous);
	return rc;
}

static int vmem_vma_find_range(struct uk_vas *vas, __vaddr_t *vaddr, __sz *len,
			       struct uk_vma **start, struct uk_vma **end,
			       int strict)
{
	struct uk_vma *vma_start, *vma_end, *next;
	__vaddr_t vstart, vend;
	__sz vl;
	int algn_lvl;

	UK_ASSERT(vas);
	UK_ASSERT(start);
	UK_ASSERT(end);

	UK_ASSERT(vaddr);
	vstart = *vaddr;

	UK_ASSERT(len);
	vl = *len;

	UK_ASSERT(vstart <= __VADDR_MAX - vl);
	vend = vstart + vl;

	if (!*start) {
		vma_start = vmem_vma_find(vas, vstart, vl);
		if (unlikely(!vma_start))
			return -ENOENT;
	} else {
		vma_start = *start;
	}

	/* Adjust vstart to make sure it is within the VMA. We expect
	 * vstart to be properly aligned by the caller.
	 */
	UK_ASSERT(vstart < vma_start->end);
	if (vstart < vma_start->start) {
		if (strict)
			return -ENOENT;

		vstart = vma_start->start;
	} else if (vstart > vma_start->start) {
		algn_lvl = MAX(vma_start->page_lvl, UK_PAGING_PAGE_LEVEL);
		if (!UK_PAGING_PAGE_Lx_ALIGNED(vstart, algn_lvl))
			return -EINVAL;
	}

	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vstart, MAX(vma_start->page_lvl,
							UK_PAGING_PAGE_LEVEL)));

	/* Find the end VMA */
	vma_end = vma_start;
	if (likely(vl > 0)) {
		while (vma_end->vma_list.next != &vas->vma_list) {
			if (vend > vma_end->start && vend <= vma_end->end)
				break;

			/* In strict mode, we do not allow the specified
			 * address range to contain holes between VMAs
			 */
			next = uk_list_next_entry(vma_end, vma_list);
			if (strict && vma_end->end != next->start)
				return -ENOENT;

			if (vend <= next->start)
				break;

			vma_end = next;
		}

		/* Adjust vend to make sure it is within the VMA. We expect
		 * vend to be properly aligned by the caller (via len).
		 */
		if (vend > vma_end->end) {
			if (strict)
				return -ENOENT;

			vend = vma_end->end;
		} else if (vend < vma_end->end) {
			algn_lvl = MAX(vma_end->page_lvl, UK_PAGING_PAGE_LEVEL);
			if (!UK_PAGING_PAGE_Lx_ALIGNED(vend, algn_lvl))
				return -EINVAL;
		}
	}

	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vend, MAX(vma_end->page_lvl,
						      UK_PAGING_PAGE_LEVEL)));

	UK_ASSERT(vend > vstart);
	UK_ASSERT(vstart >= vma_start->start && vstart < vma_start->end);
	UK_ASSERT(vend > vma_end->start && vend <= vma_end->end);
	UK_ASSERT(uk_paging_vaddr_range_isvalid(vstart, vl));

	*vaddr = vstart;
	*len   = vend - vstart;
	*start = vma_start;
	*end   = vma_end;

	return 0;
}

static void vmem_vma_insert(struct uk_vas *vas, struct uk_vma *vma)
{
	struct uk_list_head *prev;
	struct uk_vma *cur;

	UK_ASSERT(vas);
	UK_ASSERT(uk_list_empty(&vma->vma_list));
	UK_ASSERT(!vmem_vma_find(vas, vma->start, vma->end - vma->start));

	prev = &vas->vma_list;
	uk_list_for_each_entry(cur, &vas->vma_list, vma_list) {
		if (vma->start < cur->end) {
			UK_ASSERT(vma->end <= cur->start);

			uk_list_add(&vma->vma_list, prev);
			return;
		}

		prev = &cur->vma_list;
	}

	uk_list_add_tail(&vma->vma_list, &vas->vma_list);
}

static inline int vmem_vma_can_merge(struct uk_vma *vma, struct uk_vma *next)
{
	UK_ASSERT(vma);
	UK_ASSERT(next);
	UK_ASSERT(vma->vas == next->vas);

	return ((vma->end == next->start) &&
		(vma->ops == next->ops) &&
		(vma->owner == next->owner) &&
		(vma->attr == next->attr) &&
		(vma->flags == next->flags) &&
		(vma->page_lvl == next->page_lvl) &&
		(vma->name == next->name));
}

static int vmem_vma_do_try_merge_with_next(struct uk_vma *vma)
{
	struct uk_vma *next;
	int rc;

	UK_ASSERT(vma);

	if (vma->vma_list.next == &vma->vas->vma_list)
		return -ENOENT;

	next = uk_list_next_entry(vma, vma_list);

	if (!vmem_vma_can_merge(vma, next))
		return -EPERM;

	rc = VMA_MERGE(vma, next);
	if (unlikely(rc))
		return rc;

	/* Expand the VMA to include the next VMA */
	vma->end = next->end;

	/* Remove and destroy the next VMA. However, we keep the mapping! */
	vmem_vma_unlink_and_free(next);

	return 0;
}

static struct uk_vma *vmem_vma_try_merge_with_next(struct uk_vma *vma)
{
	vmem_vma_do_try_merge_with_next(vma);
	return vma;
}

static struct uk_vma *vmem_vma_try_merge_with_prev(struct uk_vma *vma)
{
	struct uk_vma *prev;

	if (vma->vma_list.prev == &vma->vas->vma_list)
		return vma;

	prev = uk_list_prev_entry(vma, vma_list);
	return (vmem_vma_do_try_merge_with_next(prev) == 0) ? prev : vma;
}

static struct uk_vma *vmem_vma_try_merge(struct uk_vma *vma)
{
	vmem_vma_try_merge_with_next(vma);
	return vmem_vma_try_merge_with_prev(vma);
}

static int vmem_vma_split(struct uk_vma *vma, __vaddr_t vaddr,
			  struct uk_vma **new_vma)
{
	struct uk_vma *v = __NULL;
	int rc;

	UK_ASSERT(vma);
	UK_ASSERT(new_vma);

	UK_ASSERT(vaddr >= vma->start && vaddr < vma->end);
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vaddr, MAX(vma->page_lvl,
						       UK_PAGING_PAGE_LEVEL)));
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vma->end - vaddr,
					    MAX(vma->page_lvl,
						UK_PAGING_PAGE_LEVEL)));

	vmem_mutation_pause(vma->vas);
	rc = VMA_SPLIT(vma, vaddr, &v);
	if (!rc && !v) {
		v = uk_malloc(vma->vas->a, sizeof(struct uk_vma));
		if (unlikely(!v))
			rc = -ENOMEM;
	}
	vmem_mutation_resume(vma->vas);
	if (unlikely(rc))
		return rc;

	UK_ASSERT(v);

	UK_INIT_LIST_HEAD(&v->vma_list);
	v->start	= vaddr;
	v->end		= vma->end;
	v->vas		= vma->vas;
	v->ops		= vma->ops;
	v->owner	= vma->owner;
	v->attr		= vma->attr;
	v->flags	= vma->flags;
	v->page_lvl	= vma->page_lvl;
	v->name		= vma->name;

	vma->end	= vaddr;

	uk_list_add(&v->vma_list, &vma->vma_list);

	*new_vma = v;
	return 0;
}

static inline int vmem_vma_need_split(struct uk_vma *vma, __sz len,
				      unsigned long *attr)
{
	UK_ASSERT(vma);

	return (len > 0) && (!attr || (vma->attr != *attr));
}

static int vmem_vma_split_vmas(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
			       unsigned long *attr, struct uk_vma **start,
			       struct uk_vma **end, int strict)
{
	struct uk_vma *vma_start = __NULL, *vma_end, *vma;
	__vaddr_t vend;
	__sz left, right;
	int rc;

	UK_ASSERT(vas);
	UK_ASSERT(start);
	UK_ASSERT(end);

	/*
	 * We can have the following cases:
	 *
	 * AAAAAAAAA        AAAAAAAAA         AAAAAAAAA         AAAAAAAAA
	 * ^-------^        ^---^                 ^---^           ^---^
	 *
	 * AAA C BBB        AAA C BBB         AAA C BBB         AAA C BBB
	 * ^-------^         ^------^         ^------^           ^-----^
	 */
	rc = vmem_vma_find_range(vas, &vaddr, &len, &vma_start, &vma_end,
				 strict);
	if (unlikely(rc))
		return rc;

	vend  = vaddr + len;
	left  = vaddr - vma_start->start;
	right = vma_end->end - vend;

	if (vmem_vma_need_split(vma_start, left, attr)) {
		rc = vmem_vma_split(vma_start, vaddr, &vma);
		if (unlikely(rc))
			return rc;

		UK_ASSERT(vma);

		if (vma_start == vma_end)
			vma_end = vma;

		vma_start = vma;
	}

	if (likely(len > 0)) {
		if (vmem_vma_need_split(vma_end, right, attr)) {
			rc = vmem_vma_split(vma_end, vend, &vma);
			if (unlikely(rc)) {
				vmem_vma_try_merge(vma_start);
				return rc;
			}

			UK_ASSERT(vma);
		}
	} else {
		UK_ASSERT(vma_start == vma_end);
	}

	*start = vma_start;
	*end   = vma_end;

	return 0;
}

/* Argument list is empty to accept (and ignore) any number of args.
 *
 * Do not convert to `uk_vma_op_deny(void)` no matter how much your static code
 * checker screams at you.
 */
int uk_vma_op_deny()
{
	return -EPERM;
}

void uk_vma_op_fault_discard(struct uk_vma *vma, struct uk_vm_fault *fault)
{
	struct uk_falloc *fa = vma->vas->pt->fa;

	fa->ffree(fa, fault->paddr, fault->len / UK_PAGING_PAGE_SIZE);
}

int uk_vma_op_unmap(struct uk_vma *vma, __vaddr_t vaddr, __sz len)
{
	UK_ASSERT(vaddr >= vma->start);
	UK_ASSERT(vaddr + len <= vma->end);
	UK_ASSERT(UK_PAGING_PAGE_ALIGNED(len));

	return uk_paging_page_unmap(vma->vas->pt, vaddr,
				    len / UK_PAGING_PAGE_SIZE, 0);
}

static void vmem_vma_unmap(struct uk_vma *vma, __vaddr_t vaddr, __sz len)
{
	int rc;

	UK_ASSERT(vma);

	UK_ASSERT(vaddr >= vma->start && vaddr < vma->end);
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vaddr, MAX(vma->page_lvl,
						       UK_PAGING_PAGE_LEVEL)));
	UK_ASSERT(vaddr <= __VADDR_MAX - len);
	UK_ASSERT((vaddr + len) > vma->start && (vaddr + len) <= vma->end);
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(len, MAX(vma->page_lvl,
						     UK_PAGING_PAGE_LEVEL)));
	UK_ASSERT(len > 0);

	rc = VMA_UNMAP(vma, vaddr, len);
	if (unlikely(rc)) {
		/* We consider a failure to unmap an address range as fatal
		 * to simplify error handling.
		 */
		UK_CRASH("Failed to unmap address range 0x%" __PRIvaddr
			 "-0x%" __PRIvaddr ": %d", vaddr, vaddr + len, rc);
	}
}

static void vmem_vma_unmap_and_free(struct uk_vma *vma)
{
	vmem_vma_unmap(vma, vma->start, uk_vma_len(vma));
	vmem_vma_destroy(vma);
}

static void vmem_vma_unmap_and_free_vmas(struct uk_vma *start,
					 struct uk_vma *end)
{
	struct uk_vma *vma = start, *next;

	UK_ASSERT(start);
	UK_ASSERT(end);

	/* It is safe now to unmap and free the VMAs */
	while (vma != end) {
		next = uk_list_next_entry(vma, vma_list);
		vmem_vma_unmap_and_free(vma);

		vma = next;
	}

	vmem_vma_unmap_and_free(end);
}

static int vmem_unmap(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		 unsigned long flags)
{
	struct uk_vma *vma_start = __NULL, *vma_end;
	int strict = (flags & UK_VMA_FLAG_STRICT_VMA_CHECK);
	int rc;

	if (unlikely(len == 0))
		return 0;

	rc = vmem_vma_split_vmas(vas, vaddr, len, __NULL,
				 &vma_start, &vma_end, strict);
	if (unlikely(rc)) {
		if (rc == -ENOENT && !strict)
			return 0;

		return rc;
	}

	/* Unlink all VMAs starting from vma_start to vma_end */
	vma_start->vma_list.prev->next = vma_end->vma_list.next;
	vma_end->vma_list.next->prev   = vma_start->vma_list.prev;

	vmem_vma_unmap_and_free_vmas(vma_start, vma_end);

	return 0;
}

int uk_vma_unmap(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		 unsigned long flags)
{
	unsigned long previous;
	int rc;

	previous = vmem_mutation_begin(vas);
	rc = vmem_unmap(vas, vaddr, len, flags);
	vmem_mutation_end(vas, previous);
	return rc;
}

static __vaddr_t vmem_first_fit(struct uk_vas *vas, __vaddr_t base, __sz align,
				__sz len)
{
	__vaddr_t vaddr = base;
	struct uk_vma *cur;

	UK_ASSERT(vas);

	/* Since we are scanning the VAS for an empty address range, we need
	 * to be careful not to overflow. Checks are thus always active and not
	 * just asserts.
	 */
	uk_list_for_each_entry(cur, &vas->vma_list, vma_list) {
		if (unlikely(vaddr > __VADDR_MAX - align))
			return UK_PAGING_VADDR_INV;

		vaddr = ALIGN_UP(vaddr, align);

		if (unlikely(vaddr > __VADDR_MAX - len))
			return UK_PAGING_VADDR_INV;

		if (vaddr + len <= cur->start)
			return vaddr;

		vaddr = MAX(cur->end, base);
	}

	if (unlikely(vaddr > __VADDR_MAX - align))
		return UK_PAGING_VADDR_INV;

	return ALIGN_UP(vaddr, align);
}

static int vmem_mapx_populate(struct uk_pagetable *pt __unused,
			      __vaddr_t vaddr, __vaddr_t pt_vaddr,
			      unsigned int level, __pte_t *pte, void *user)
{
	struct uk_vma *vma = (struct uk_vma *)user;
	struct uk_vm_fault fault = {
		.vaddr = vaddr,
		.vbase = vaddr,
		.len   = UK_PAGING_PAGE_Lx_SIZE(level),
		.paddr = UK_PAGING_PT_Lx_PTE_PADDR(*pte, level),
		.type  = UK_VMA_FAULT_SOFT | UK_VMA_FAULT_NONPRESENT,
		.pte   = *pte,
		.level = level,
		.regs  = __NULL,
	};
	__pte_t current;
	int rc;

	UK_ASSERT(vma->ops);
	UK_ASSERT(vma->ops->fault);

	vmem_mutation_pause(vma->vas);
	rc = vma->ops->fault(vma, &fault);
	vmem_mutation_resume(vma->vas);
	if (unlikely(rc)) {
		if (rc == -ENOMEM)
			return UK_PAGING_PAGE_MAPX_ETOOBIG;

		return rc;
	}

	/* Mutators retain the mutex across callbacks, so parent tables survive.
	 * A demand fault can install a leaf or child table while we prepare data.
	 */
	rc = uk_paging_pte_read(pt_vaddr, level,
				UK_PAGING_PT_Lx_IDX(vaddr, level), &current);
	if (rc || UK_PAGING_PT_Lx_PTE_PRESENT(current, level)) {
		if (vma->ops->fault_discard)
			vma->ops->fault_discard(vma, &fault);
		if (rc)
			return rc;
		return UK_PAGING_PAGE_Lx_IS(current, level) ?
			UK_PAGING_PAGE_MAPX_ESKIP : UK_PAGING_PAGE_MAPX_ETOOBIG;
	}

	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(fault.paddr, fault.level));

	*pte = UK_PAGING_PT_Lx_PTE_SET_PADDR(fault.pte, fault.level,
					     fault.paddr);

	return 0;
}

static int vmem_mapx_initial_populate(struct uk_pagetable *pt,
		__vaddr_t vaddr, __vaddr_t pt_vaddr, unsigned int level,
		__pte_t *pte, void *user)
{
	int rc = vmem_mapx_populate(pt, vaddr, pt_vaddr, level, pte, user);

	return rc == -ENXIO ? UK_PAGING_PAGE_MAPX_ESKIP : rc;
}

static int vmem_map(struct uk_vas *vas, __vaddr_t *vaddr, __sz len,
	       unsigned long attr, unsigned long flags, const char *name,
	       const struct uk_vma_ops *ops, void *args, const void *owner)
{
	unsigned int order = UK_VMA_MAP_SIZE_TO_ORDER(flags);
	int rc, to_lvl, algn_lvl, strict;
	struct uk_vma *vma_start = __NULL;
	struct uk_vma *vma_end = __NULL;
	struct uk_vma *vma = __NULL;
	unsigned long extf;
	unsigned long flgs;
	__vaddr_t va, base;

	UK_ASSERT(vas);
	UK_ASSERT(vaddr);
	UK_ASSERT(ops);
	UK_ASSERT(len > 0);

	if (order > 0) {
		UK_ASSERT(order >= 12);

		to_lvl   = UK_PAGING_PAGE_SHIFT_Lx(order);
		algn_lvl = to_lvl;

		UK_ASSERT(UK_PAGING_PAGE_Lx_HAS(algn_lvl));
	} else {
		to_lvl   = -1;
		algn_lvl = UK_PAGING_PAGE_LEVEL;
	}

	if (unlikely(!UK_PAGING_PAGE_Lx_ALIGNED(len, algn_lvl)))
		return -EINVAL;

	va = *vaddr;
	if (va == UK_PAGING_VADDR_ANY || (flags & UK_VMA_MAP_SEARCH)) {
		/* Select the first virtual address range starting at the
		 * mapping base that can accommodate the requested VMA.
		 */
		base = va;
		if (va == UK_PAGING_VADDR_ANY) {
			vmem_mutation_pause(vas);
			base = (ops->get_base) ? ops->get_base(vas, args, flags) :
						 vas->vma_base;
			vmem_mutation_resume(vas);
		}

		va = vmem_first_fit(vas, base, UK_PAGING_PAGE_Lx_SIZE(algn_lvl),
				    len);
		if (unlikely(va == UK_PAGING_VADDR_INV))
			return -ENOMEM;
	} else {
		if (unlikely(!UK_PAGING_PAGE_Lx_ALIGNED(va, algn_lvl)))
			return -EINVAL;
		if (unlikely(!uk_paging_vaddr_range_isvalid(va, len)))
			return -ENOMEM;

		/* The caller specified an exact address to map to. We only
		 * allow this if the mapping does not collide with other
		 * mappings. However, we enforce the new mapping if the replace
		 * flag is provided by unmapping everything in the range first.
		 */
		if ((vma_start = vmem_vma_find(vas, va, len))) {
			if (unlikely(!(flags & UK_VMA_MAP_REPLACE)))
				return -EEXIST;

			strict = (flags & UK_VMA_FLAG_STRICT_VMA_CHECK);
			rc = vmem_vma_split_vmas(vas, va, len, __NULL,
						 &vma_start, &vma_end, strict);
			if (unlikely(rc))
				return rc;

			/* It could be that during the split operation we
			 * recognized that len must be aligned to a larger
			 * page size to allow the split.
			 */
			UK_ASSERT(vma_end->end > vma_start->start);
			len = vma_end->end - vma_start->start;
		}
	}

	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(va, algn_lvl));
	UK_ASSERT(va <= __VADDR_MAX - len);

	/* Applications can request invalid memory ranges in mmap. In case the
	 * address is not valid, then ENOMEM is the specfied error code.
	 * This should only happen rarely in practice, for example when JS
	 * engines (mozjs) do weird stuff to figure out the available address
	 * bits.
	 */
	if (unlikely(!uk_paging_vaddr_range_isvalid(va, len)))
		return -ENOMEM;

	/* Create a new VMA for the requested range. */
	vmem_mutation_pause(vas);
	if (ops->new) {
		/* Split also needs to allocate a new VMA */
		UK_ASSERT(ops->split);

		rc = ops->new(vas, va, len, args, attr, &flags, &vma);
	} else {
		vma = uk_malloc(vas->a, sizeof(struct uk_vma));
		rc = vma ? 0 : -ENOMEM;
		if (vma)
			vma->name = __NULL;
	}
	vmem_mutation_resume(vas);
	if (unlikely(rc))
		return rc;

	UK_ASSERT(vma);

	/* If this mapping replaces existing VMAs we now have to unmap them
	 * before we attempt to map the new VMA because this entail changes to
	 * the page table.
	 */
	if (vma_start) {
		UK_ASSERT(vma_end);

		/* Unlink all VMAs starting from vma_start to vma_end */
		vma_start->vma_list.prev->next = vma_end->vma_list.next;
		vma_end->vma_list.next->prev   = vma_start->vma_list.prev;

		vmem_vma_unmap_and_free_vmas(vma_start, vma_end);
	}

	UK_ASSERT(vmem_vma_find(vas, va, len) == __NULL);

	/* Extract extended flags */
	extf = flags & (UK_VMA_MAP_EXTF_MASK << UK_VMA_MAP_EXTF_SHIFT);

	UK_INIT_LIST_HEAD(&vma->vma_list);
	vma->start	= va;
	vma->end	= va + len;
	vma->vas	= vas;
	vma->ops	= ops;
	vma->owner	= owner;
	vma->attr	= attr;
	vma->flags	= extf;
	vma->page_lvl	= to_lvl;
	if (name)
		vma->name = name;

	if (flags & UK_VMA_MAP_PRIVATE)
		vma->flags |= UK_VMA_FLAG_PRIVATE;

	if (flags & UK_VMA_MAP_UNINITIALIZED)
		vma->flags |= UK_VMA_FLAG_UNINITIALIZED;

	if (flags & UK_VMA_MAP_POPULATE) {
		UK_ASSERT(vma->ops->fault);

		if (vma->page_lvl >= 0) {
			/* Enforce desired page size when populating the VMA */
			flgs = UK_PAGING_PAGE_FLAG_SIZE(vma->page_lvl) |
				UK_PAGING_PAGE_FLAG_FORCE_SIZE;
		} else {
			flgs = 0;
		}

		rc = uk_paging_page_mapx(vas->pt, vma->start, 0,
					 len >> UK_PAGING_PAGE_Lx_SHIFT(algn_lvl),
					 vma->attr, flgs,
					 &(struct uk_paging_page_mapx){
						.map = vmem_mapx_initial_populate,
						.ctx = vma,
					 });
		if (unlikely(rc)) {
			/* If the address range replaces existing mappings, we
			 * have an unrecoverable error and the address range
			 * will be empty!
			 */
			vmem_vma_destroy(vma);

			return rc;
		}
	}

	vmem_vma_insert(vas, vma);
	vmem_vma_try_merge(vma);

	*vaddr = va;
	return 0;
}

int uk_vma_map(struct uk_vas *vas, __vaddr_t *vaddr, __sz len,
	       unsigned long attr, unsigned long flags, const char *name,
	       const struct uk_vma_ops *ops, void *args, const void *owner)
{
	unsigned long previous;
	int rc;

	previous = vmem_mutation_begin(vas);
	rc = vmem_map(vas, vaddr, len, attr, flags, name, ops, args, owner);
	vmem_mutation_end(vas, previous);
	return rc;
}

int uk_vma_op_set_attr(struct uk_vma *vma, unsigned long attr)
{
	unsigned long pgs = uk_vma_len(vma) / UK_PAGING_PAGE_SIZE;

	return uk_paging_page_set_attr(vma->vas->pt, vma->start, pgs, attr, 0);
}

static int vmem_vma_set_attr(struct uk_vma *vma, unsigned long attr)
{
	int rc;

	UK_ASSERT(vma);

	if (vma->attr == attr)
		return 0;

	/* Do not change attributes if there is no VMA handler for this */
	if (!vma->ops->set_attr)
		return 0;

	rc = VMA_SETATTR(vma, attr);
	if (unlikely(rc))
		return rc;

	vma->attr = attr;
	return 0;
}

static int vmem_vma_set_attr_vmas(struct uk_vma *start, struct uk_vma *end,
				   unsigned long attr)
{
	struct uk_vma *vma = start;

	UK_ASSERT(start);
	UK_ASSERT(end);

	while (vma != end) {
		int rc = vmem_vma_set_attr(vma, attr);
		if (rc)
			return rc;
		vma = uk_list_next_entry(vma, vma_list);
	}

	int rc = vmem_vma_set_attr(end, attr);
	if (rc)
		return rc;

	/* Do a second pass and try to merge VMAs */
	vma = vmem_vma_try_merge_with_next(end);
	UK_ASSERT(vma == end);

	vma = start;
	while (vma != end) {
		vma = vmem_vma_try_merge_with_prev(vma);
		vma = uk_list_next_entry(vma, vma_list);
	}

	vmem_vma_try_merge_with_prev(end);
	return 0;
}

static int vmem_set_attr(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		    unsigned long attr, unsigned long flags)
{
	struct uk_vma *vma_start = __NULL, *vma_end;
	int strict = (flags & UK_VMA_FLAG_STRICT_VMA_CHECK);
	int rc;

	if (unlikely(len == 0))
		return 0;

	rc = vmem_vma_split_vmas(vas, vaddr, len, &attr,
				 &vma_start, &vma_end, strict);
	if (unlikely(rc)) {
		if (rc == -ENOENT && !strict)
			return 0;

		return rc;
	}

	return vmem_vma_set_attr_vmas(vma_start, vma_end, attr);
}

int uk_vma_set_attr(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		    unsigned long attr, unsigned long flags)
{
	unsigned long previous;
	int rc;

	previous = vmem_mutation_begin(vas);
	rc = vmem_set_attr(vas, vaddr, len, attr, flags);
	vmem_mutation_end(vas, previous);
	return rc;
}

static int vmem_mapx_advise(struct uk_pagetable *pt,
			    __vaddr_t vaddr, __vaddr_t pt_vaddr,
			    unsigned int level, __pte_t *pte, void *user)
{
	__pte_t opte;
	int rc;

	/* With advise it can happen that we get calls for pages already
	 * present. Just ignore these.
	 */
	rc = uk_paging_pte_read(pt_vaddr, level,
				UK_PAGING_PT_Lx_IDX(vaddr, level), &opte);
	if (unlikely(rc))
		return rc;

	if (UK_PAGING_PT_Lx_PTE_PRESENT(opte, level))
		return UK_PAGING_PAGE_MAPX_ESKIP;

	return vmem_mapx_populate(pt, vaddr, pt_vaddr, level, pte, user);
}

int uk_vma_op_advise(struct uk_vma *vma, __vaddr_t vaddr, __sz len,
		     unsigned long advice)
{
	unsigned int lvl;
	unsigned long flgs;
	int rc;

	/* WILLNEED takes precedence over DONTNEED */
	if (advice & UK_VMA_ADV_WILLNEED) {
		if (vma->page_lvl >= 0) {
			flgs = UK_PAGING_PAGE_FLAG_SIZE(vma->page_lvl) |
				UK_PAGING_PAGE_FLAG_FORCE_SIZE;
			lvl  = vma->page_lvl;
		} else {
			flgs = 0;
			lvl  = UK_PAGING_PAGE_LEVEL;
		}

		rc = uk_paging_page_mapx(vma->vas->pt, vaddr, 0,
					 len >> UK_PAGING_PAGE_Lx_SHIFT(lvl),
					 vma->attr, flgs,
					 &(struct uk_paging_page_mapx){
						.map = vmem_mapx_advise,
						.ctx = vma,
					 });
		if (unlikely(rc))
			return rc;
	} else if (advice & UK_VMA_ADV_DONTNEED) {
		/* Note: We are using the same semantic as Linux here, where
		 * DONTNEED means we will actually free the physical memory and
		 * not only swap it out.
		 */
		rc = uk_vma_op_unmap(vma, vaddr, len);
		if (unlikely(rc))
			return rc;
	}

	/* Add further advices here */

	return 0;
}

static int vmem_vma_advise(struct uk_vma *vma, __vaddr_t vaddr, __sz len,
			   unsigned long advice)
{
	UK_ASSERT(vma);
	UK_ASSERT(vaddr <= __VADDR_MAX - len);
	UK_ASSERT(vaddr >= vma->start && vaddr + len <= vma->end);
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(vaddr, MAX(vma->page_lvl,
						       UK_PAGING_PAGE_LEVEL)));
	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(len, MAX(vma->page_lvl,
						     UK_PAGING_PAGE_LEVEL)));

	return VMA_ADVISE(vma, vaddr, len, advice);
}

static int vmem_advise(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		  unsigned long advice, unsigned long flags)
{
	struct uk_vma *vma_start = __NULL, *vma_end, *vma;
	int strict = (flags & UK_VMA_FLAG_STRICT_VMA_CHECK);
	__vaddr_t vend;
	int rc;

	if (unlikely(len == 0))
		return 0;

	rc = vmem_vma_find_range(vas, &vaddr, &len,
				 &vma_start, &vma_end, strict);
	if (unlikely(rc)) {
		if (rc == -ENOENT && !strict)
			return 0;

		return rc;
	}

	vend = vaddr + len;
	vma  = vma_start;
	while (vma != vma_end) {
		rc = vmem_vma_advise(vma, vaddr, vma->end - vaddr, advice);
		if (unlikely(rc))
			return rc;

		vma   = uk_list_next_entry(vma, vma_list);
		vaddr = vma->start;
	}

	return vmem_vma_advise(vma, vaddr, vend - vaddr, advice);
}

int uk_vma_sync(struct uk_vas *vas, __vaddr_t addr, __sz len)
{
	struct uk_vma *first = NULL, *last, *vma;
	unsigned long saved;
	int rc;

	if (!len)
		return 0;
	saved = vmem_mutation_begin(vas);
	rc = vmem_vma_find_range(vas, &addr, &len, &first, &last, 1);
	if (rc)
		goto out;
	const __vaddr_t end = addr + len;
	for (vma = first; ; vma = uk_list_next_entry(vma, vma_list)) {
		__vaddr_t stop = MIN(vma->end, end);
		if (vma->ops->sync) {
			vmem_mutation_pause(vas);
			rc = vma->ops->sync(vma, addr, stop - addr);
			vmem_mutation_resume(vas);
			if (rc)
				break;
		}
		if (vma == last)
			break;
		addr = vma->end;
	}
out:
	vmem_mutation_end(vas, saved);
	return rc;
}

int uk_vma_populate_page(struct uk_vas *vas, __vaddr_t vaddr)
{
	struct uk_vma *vma;
	unsigned long previous;
	unsigned int level;
	int rc = -ENOENT;
#ifdef CONFIG_LIBUKPAGING
	__pte_t entry;
	unsigned long flags = uk_pal_save_irqf();
	int present;

	level = UK_PAGING_PAGE_LEVEL;
	vmem_fault_lock(vas);
	present = !uk_paging_pt_walk(vas->pt, vaddr, &level, NULL, &entry) &&
		UK_PAGING_PT_Lx_PTE_PRESENT(entry, level) &&
		UK_PAGING_PAGE_Lx_IS(entry, level);
	uk_spin_unlock_irqrestore(&vas->fault_lock, flags);
	if (present)
		return 0;
#endif

	previous = vmem_mutation_begin(vas);
	vma = vmem_vma_find(vas, vaddr, 0);
	if (vma) {
		level = MAX(vma->page_lvl, UK_PAGING_PAGE_LEVEL);
		rc = vmem_vma_advise(vma,
			UK_PAGING_PAGE_Lx_ALIGN_DOWN(vaddr, level),
			UK_PAGING_PAGE_Lx_SIZE(level), UK_VMA_ADV_WILLNEED);
	}
	vmem_mutation_end(vas, previous);
	return rc;
}

int uk_vma_advise(struct uk_vas *vas, __vaddr_t vaddr, __sz len,
		  unsigned long advice, unsigned long flags)
{
	unsigned long previous;
	int rc;

	previous = vmem_mutation_begin(vas);
	rc = vmem_advise(vas, vaddr, len, advice, flags);
	vmem_mutation_end(vas, previous);
	return rc;
}

#ifdef CONFIG_LIBUKPAGING
static inline int vmem_largest_level(__vaddr_t vaddr, __sz len,
				   unsigned int max_lvl)
{
	unsigned int lvl = max_lvl;

	while (lvl > UK_PAGING_PAGE_LEVEL) {
		if (UK_PAGING_PAGE_Lx_HAS(lvl) &&
		    UK_PAGING_PAGE_Lx_ALIGNED(vaddr, lvl) &&
		    UK_PAGING_PAGE_Lx_SIZE(lvl) <= len)
			return lvl;

		lvl--;
	}

	return lvl;
}

static inline int vmem_access_allowed(unsigned long attr,
				      unsigned int faulttype)
{
	switch (faulttype & UK_VMA_FAULT_ACCESSTYPE) {
	case UK_VMA_FAULT_READ:
		return (attr & UK_PAGING_PAGE_ATTR_PROT_READ);
	case UK_VMA_FAULT_WRITE:
		return (attr & UK_PAGING_PAGE_ATTR_PROT_WRITE);
	case UK_VMA_FAULT_EXEC:
		return (attr & UK_PAGING_PAGE_ATTR_PROT_EXEC);
	}

	return 0;
}

struct mapx_pagefault_ctx {
	/** Faulting virtual address */
	__vaddr_t vaddr;
	/** Type of the fault */
	unsigned int type;
	/** Trap frame */
	struct uk_lcpu_regs *regs;
	/** VMA in which the fault happens */
	struct uk_vma *vma;
};

static int vmem_mapx_pagefault(struct uk_pagetable *pt __unused,
			       __vaddr_t vaddr, __vaddr_t pt_vaddr __unused,
			       unsigned int level, __pte_t *pte, void *user)
{
	struct mapx_pagefault_ctx *ctx = (struct mapx_pagefault_ctx *)user;
	struct uk_vm_fault fault = {
		.vaddr = ctx->vaddr,
		.vbase = vaddr,
		.len   = UK_PAGING_PAGE_Lx_SIZE(level),
		.paddr = UK_PAGING_PT_Lx_PTE_PADDR(*pte, level),
		.type  = ctx->type,
		.pte   = *pte,
		.level = level,
		.regs  = ctx->regs,
	};
	int rc;

	UK_ASSERT(ctx->vma->ops);
	UK_ASSERT(ctx->vma->ops->fault);

	rc = ctx->vma->ops->fault(ctx->vma, &fault);
	if (unlikely(rc)) {
		if (rc == -ENOMEM)
			return UK_PAGING_PAGE_MAPX_ETOOBIG;

		return rc;
	}

	UK_ASSERT(UK_PAGING_PAGE_Lx_ALIGNED(fault.paddr, fault.level));

	*pte = UK_PAGING_PT_Lx_PTE_SET_PADDR(fault.pte, fault.level,
					     fault.paddr);

	return 0;
}

static int vmem_pagefault_locked(struct uk_vas *vas, __vaddr_t vaddr,
				 unsigned int type, struct uk_lcpu_regs *regs)
{
	const unsigned int demand_lvl =
		UK_PAGING_PAGE_SHIFT_Lx(CONFIG_LIBUKVMEM_DEMAND_PAGE_IN_SIZE);
	struct uk_pagetable *pt;
	__pte_t pte;
	struct mapx_pagefault_ctx ctx = {
		.vaddr = vaddr,
		.type  = type,
		.regs  = regs,
	};
	struct uk_paging_page_mapx mapx = {
		.map = vmem_mapx_pagefault,
		.ctx = &ctx,
	};
	__vaddr_t vbase;
	unsigned int lvl = UK_PAGING_PAGE_LEVEL;
	unsigned long flags;
	int rc;

	if (unlikely(vas->flags & UK_VAS_FLAG_NO_PAGING))
		return -EFAULT;

	/* If the page fault was caused by an access to a region not covered by
	 * a virtual memory area, fail early.
	 */
	ctx.vma = vmem_vma_find(vas, vaddr, 0);
	if (unlikely(!ctx.vma))
		return -EFAULT;

	UK_ASSERT(vaddr >= ctx.vma->start && vaddr < ctx.vma->end);

	/* Fail if the access is not compatible with the VMA's settings */
	if (unlikely(!vmem_access_allowed(ctx.vma->attr, type)))
		return -EFAULT;

	/* Fail early if the VMA does not have a fault handler */
	if (unlikely(!ctx.vma->ops->fault))
		return -EFAULT;

	UK_ASSERT(ctx.vma->vas);
	UK_ASSERT(ctx.vma->vas->pt);
	pt = ctx.vma->vas->pt;
	if (type & UK_VMA_FAULT_NONPRESENT) {
		rc = uk_paging_pt_walk(pt, vaddr, &lvl, __NULL, &pte);
		if (unlikely(rc))
			return rc;
		/* A different CPU can install this page before we take the lock. */
		if (UK_PAGING_PT_Lx_PTE_PRESENT(pte, lvl) &&
		    UK_PAGING_PAGE_Lx_IS(pte, lvl))
			return 0;
		lvl = UK_PAGING_PAGE_LEVEL;
	}

	if (ctx.vma->ops->fault_can_block)
		return (type & UK_VMA_FAULT_NONPRESENT) ? -EAGAIN : -EFAULT;

	/* Find the page level at which we want to page-in. If the VMA does not
	 * enforce a specific page size and the configuration allows to page-in
	 * large pages, we first check up to which level we find page tables.
	 * We cannot create pages larger than that. Afterwards, we adjust
	 * according to alignment and VMA boundaries.
	 */
	if (ctx.vma->page_lvl < 0 && demand_lvl > UK_PAGING_PAGE_LEVEL) {
		rc = uk_paging_pt_walk(pt, vaddr, &lvl, __NULL, __NULL);
		if (unlikely(rc))
			return rc;

		vbase = MAX(UK_PAGING_PAGE_Lx_ALIGN_DOWN(vaddr, lvl),
			    ctx.vma->start);

		lvl = vmem_largest_level(vbase, ctx.vma->end - vbase,
					 MIN(lvl, demand_lvl));

		flags = UK_PAGING_PAGE_FLAG_FORCE_SIZE;
	} else {
		lvl   = MAX(ctx.vma->page_lvl, UK_PAGING_PAGE_LEVEL);
		flags = 0;
	}

	vbase = UK_PAGING_PAGE_Lx_ALIGN_DOWN(vaddr, lvl);

	UK_ASSERT(vbase >= ctx.vma->start &&
		  vbase < ctx.vma->end);
	UK_ASSERT(vbase <= __VADDR_MAX - UK_PAGING_PAGE_Lx_SIZE(lvl));
	UK_ASSERT(vbase + UK_PAGING_PAGE_Lx_SIZE(lvl) >= ctx.vma->start &&
		  vbase + UK_PAGING_PAGE_Lx_SIZE(lvl) <= ctx.vma->end);

	return uk_paging_page_mapx(pt, vbase, 0, 1, ctx.vma->attr,
				UK_PAGING_PAGE_FLAG_SIZE(lvl) | flags, &mapx);
}

int vmem_pagefault(__vaddr_t vaddr, unsigned int type,
		   struct uk_lcpu_regs *regs)
{
	struct uk_vas *vas = uk_vas_get_active();
	unsigned long flags;
	int rc;

	if (unlikely(!vas))
		return -EFAULT;
	flags = uk_pal_save_irqf();
	vmem_fault_lock(vas);
	rc = vmem_pagefault_locked(vas, vaddr, type, regs);
	uk_spin_unlock_irqrestore(&vas->fault_lock, flags);
	return rc;
}
#endif /* CONFIG_LIBUKPAGING */
