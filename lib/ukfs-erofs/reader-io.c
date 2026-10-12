/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <erofs/internal.h>
#include <erofs/config.h>
#include <uk/print.h>

struct erofs_configure cfg = { .c_dbg_lvl = 0 };

void erofs_msg(int level __unused, const char *format, ...)
{
	char message[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	uk_pr_err("%s", message);
}

ssize_t erofs_io_pread(struct erofs_vfile *file, void *buffer,
		       size_t length, u64 offset)
{
	if (!file->ops || !file->ops->pread)
		return -EOPNOTSUPP;
	return file->ops->pread(file, buffer, length, offset);
}

ssize_t erofs_dev_read(struct erofs_sb_info *volume, int device,
		      void *buffer, u64 offset, size_t length)
{
	ssize_t result;

	if (device)
		return -EOPNOTSUPP;
	result = erofs_io_pread(&volume->bdev, buffer, length, offset);
	if (result < 0)
		return result;
	return (size_t)result == length ? 0 : -EIO;
}

int erofs_packedfile_read(struct erofs_sb_info *volume, void *buffer,
			erofs_off_t length, erofs_off_t offset)
{
	struct erofs_inode inode = { .sbi = volume, .nid = volume->packed_nid };
	struct erofs_vfile file;
	int result;

	result = erofs_read_inode_from_disk(&inode);
	if (result)
		return result;
	if (offset > inode.i_size || length > inode.i_size - offset)
		return -EFSCORRUPTED;
	result = erofs_iopen(&file, &inode);
	if (result)
		return result;
	return erofs_pread(&file, buffer, length, offset);
}
