/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UKVMEM_STACK_H__
#define __UKVMEM_STACK_H__

#include <uk/vmem.h>

struct uk_vma_stack_context {
	__vaddr_t start;
	__vaddr_t end;
	unsigned int references;
};

struct uk_vma_stack {
	struct uk_vma vma;
	struct uk_vma_stack_context *stack;
};

static inline struct uk_vma_stack_context *
vmem_stack_context(const struct uk_vma *vma)
{
	UK_ASSERT(vma->ops == &uk_vma_stack_ops);
	return __containerof(vma, struct uk_vma_stack, vma)->stack;
}

static inline void vmem_stack_release(struct uk_vas *vas,
				      struct uk_vma_stack_context *stack)
{
	if (!--stack->references)
		uk_free(vas->a, stack);
}

#endif
