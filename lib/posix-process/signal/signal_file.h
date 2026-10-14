/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2025, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#ifndef __UK_PROCESS_SIGNAL_H__
#error Do not include this header directly
#endif

#include <stdbool.h>
#include <uk/file.h>
#include <uk/posix-fd.h>

/* Signal file structure combining file operations with signal handling */
struct uk_signal_file {
	/* Allocator used to create this signal file */
	struct uk_alloc *a;
	/* Signal file refcount */
	uk_file_refcnt frefcnt;
	/* Signal file state */
	struct uk_file_state fstate;
	/* Base file structure */
	struct uk_file f;
	/* Signal mask for this file */
	uk_sigset_t mask;
	/* List head for tracking in signal context */
	struct uk_list_head sigf_head;
};

struct uk_signal_files_ctx {
	struct uk_list_head sigfiles;
};

static inline
void uk_signal_files_ctx_init(struct uk_signal_files_ctx *ctx)
{
	UK_INIT_LIST_HEAD(&ctx->sigfiles);
}

static inline
void uk_signal_files_ctx_add(struct uk_signal_files_ctx *ctx,
			     struct uk_signal_file *sigf)
{
	uk_list_add_tail(&sigf->sigf_head, &ctx->sigfiles);
}

static inline
void uk_signal_files_ctx_clear(struct uk_signal_files_ctx *ctx)
{
	struct uk_signal_file *file, *next;

	uk_list_for_each_entry_safe(file, next, &ctx->sigfiles, sigf_head)
		uk_list_del_init(&file->sigf_head);
}

static inline
bool uk_signal_files_ctx_is_set(struct uk_signal_files_ctx *ctx, int signum)
{
	struct uk_signal_file *file;

	uk_list_for_each_entry(file, &ctx->sigfiles, sigf_head)
		if (uk_load_n(&file->mask) & UK_BIT(signum - 1))
			return true;
	return false;
}

static inline
void uk_signal_files_ctx_notify(struct uk_signal_files_ctx *ctx,
				int signum)
{
	struct uk_signal_file *it;

	uk_list_for_each_entry(it, &ctx->sigfiles, sigf_head) {
		if (!(uk_load_n(&it->mask) & UK_BIT(signum - 1)))
			continue;
		uk_file_event_set(&it->f, UKFD_POLLIN);
	}
}

const struct uk_file *signal_file_create(struct uk_alloc *a,
					 const uk_sigset_t *mask);
