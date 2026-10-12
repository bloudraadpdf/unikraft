/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_BLKFILE_H__
#define __UK_BLKFILE_H__

#include <uk/file.h>

/* Return a borrowed file for a registered /dev/vdX device, or an error. */
const struct uk_file *uk_blkfile_open(const char *name);

#endif
