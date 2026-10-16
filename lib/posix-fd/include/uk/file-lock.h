/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_FILE_LOCK_H__
#define __UK_FILE_LOCK_H__
#include <fcntl.h>
#include <uk/file.h>

enum uk_file_lock_kind {
	UK_FILE_LOCK_PROCESS,
	UK_FILE_LOCK_DESCRIPTION,
	UK_FILE_LOCK_FLOCK
};

struct uk_file_lock_request {
	const struct uk_file *file;
	const void *owner;
	enum uk_file_lock_kind kind;
	off_t start, end;
	pid_t pid;
	short type;
};

int uk_file_lock_apply(const struct uk_file_lock_request *, struct flock *, int);
void uk_file_lock_release(const struct uk_file *file, const void *owner,
			  enum uk_file_lock_kind kind);
#endif
