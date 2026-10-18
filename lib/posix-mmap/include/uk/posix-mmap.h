/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_POSIX_MMAP_H__
#define __UK_POSIX_MMAP_H__

#include <stddef.h>
#include <sys/types.h>

struct uk_ofile;
int uk_posix_mmap_file(void **address, size_t length, int protection, int flags,
		       struct uk_ofile *file, off_t offset);

#endif
