/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

/* File mapping VMA. */

#ifndef __UK_POSIX_FILE_VMA_H__
#define __UK_POSIX_FILE_VMA_H__

#include <uk/posix-fd.h>
#include <uk/vmem/vma_types.h>

extern const struct uk_vma_ops uk_fdio_vma_ops;
struct uk_vma;

/* Caller holds the VAS mutation lock and releases the returned ofile. */
struct uk_ofile *uk_fdio_vma_file(const struct uk_vma *vma, __off *offset);

struct uk_fdio_vma_lifetime {
	void (*acquire)(struct uk_fdio_vma_lifetime *lifetime);
	void (*release)(struct uk_fdio_vma_lifetime *lifetime);
};

struct uk_fdio_vma_args {
	struct uk_ofile *of;
	__off offset;
	struct uk_fdio_vma_lifetime *lifetime;
};

int uk_fdio_unmap_lifetime(struct uk_vas *vas,
			   struct uk_fdio_vma_lifetime *lifetime);

#endif /* __UK_POSIX_FILE_VMA_H__ */
