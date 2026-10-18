/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <uk/alloc.h>
#include <uk/arch/limits.h>
#include <uk/nofault.h>
#include <uk/posix-fdio.h>
#include <uk/posix-fdtab.h>
#include <uk/syscall.h>

#define COPY_RANGE_BUFFER_SIZE 65536UL
#define COPY_RANGE_MAX_COUNT 0x7ffff000U

static int copy_range_check(struct uk_ofile *input, off_t in_offset,
			    struct uk_ofile *output, off_t out_offset,
			    size_t *count)
{
	struct uk_statx metadata[2] = {0};
	struct uk_ofile *files[] = {input, output};
	bool same_volume;
	__off maximum;
	int rc;

	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		uk_file_rlock(files[i]->file);
		rc = uk_file_getstat(files[i]->file, UK_STATX_BASIC_STATS,
				     &metadata[i]);
		uk_file_runlock(files[i]->file);
		if (rc)
			return rc;
	}
	if (S_ISDIR(metadata[0].stx_mode) || S_ISDIR(metadata[1].stx_mode))
		return -EISDIR;
	if (!S_ISREG(metadata[0].stx_mode) || !S_ISREG(metadata[1].stx_mode))
		return -EINVAL;
	if ((input->mode & O_WRONLY) ||
	    !(output->mode & (O_WRONLY | O_RDWR)) || (output->mode & O_APPEND))
		return -EBADF;
	same_volume = metadata[0].stx_dev_major == metadata[1].stx_dev_major &&
		metadata[0].stx_dev_minor == metadata[1].stx_dev_minor;
	if (!same_volume)
		return -EXDEV;
	if ((__u64)in_offset + *count < (__u64)in_offset ||
	    (__u64)out_offset + *count < (__u64)out_offset)
		return -EOVERFLOW;
	*count = (__u64)in_offset >= metadata[0].stx_size ? 0 :
		MIN(*count, metadata[0].stx_size - (__u64)in_offset);
	uk_file_rlock(output->file);
	rc = uk_file_max_size(output->file, &maximum);
	uk_file_runlock(output->file);
	if (rc)
		return rc;
	if (maximum <= 0)
		return -EOPNOTSUPP;
	if (out_offset >= maximum)
		return -EFBIG;
	if (in_offset < 0 || out_offset < 0)
		return -EINVAL;
	*count = MIN(*count, (__u64)(maximum - out_offset));
	if (metadata[0].stx_ino == metadata[1].stx_ino &&
	    (__u64)out_offset + *count > (__u64)in_offset &&
	    (__u64)out_offset < (__u64)in_offset + *count)
		return -EINVAL;
	*count = MIN(*count, COPY_RANGE_MAX_COUNT);
	return 0;
}

static ssize_t copy_range_data(struct uk_ofile *input, off_t in_offset,
			       struct uk_ofile *output, off_t out_offset,
			       size_t count)
{
	const size_t capacity = MIN(count, COPY_RANGE_BUFFER_SIZE);
	char *buffer;
	size_t copied = 0;
	ssize_t rc = 0;

	if (!count)
		return 0;
	buffer = uk_malloc(uk_alloc_get_default(), capacity);
	if (!buffer)
		return -ENOMEM;
	while (copied < count) {
		size_t written = 0;
		ssize_t received = uk_sys_pread(input, buffer,
			MIN(capacity, count - copied), in_offset + copied);

		if (received <= 0) {
			rc = received;
			break;
		}
		while (written < (size_t)received) {
			rc = uk_sys_pwrite(output, buffer + written,
					   (size_t)received - written,
					   out_offset + copied);
			if (rc <= 0)
				goto out;
			written += rc;
			copied += rc;
		}
	}
out:
	uk_free(uk_alloc_get_default(), buffer);
	return copied ? (ssize_t)copied : rc;
}

static int copy_range_publish_offset(struct uk_ofile *file, off_t *pointer,
				     off_t offset)
{
	if (pointer)
		return uk_nofault_try_write(offset, pointer) ? 0 : -EFAULT;
	file->pos = offset;
	return 0;
}

UK_SYSCALL_R_DEFINE(ssize_t, copy_file_range, int, input_fd,
		    off_t *, in_pointer, int, output_fd,
		    off_t *, out_pointer, size_t, count, unsigned int, flags)
{
	struct uk_ofile *input, *output, *first, *second;
	off_t in_offset, out_offset;
	ssize_t copied;
	ssize_t rc = -EBADF;

	input = uk_fdtab_get(input_fd);
	if (!input)
		return -EBADF;
	output = uk_fdtab_get(output_fd);
	if (!output)
		goto out_input;
	if ((input->mode | output->mode) & O_PATH)
		goto out_output;
	first = (__uptr)input < (__uptr)output ? input : output;
	second = first == input ? output : input;
	uk_mutex_lock(&first->lock);
	if (second != first)
		uk_mutex_lock(&second->lock);
	in_offset = input->pos;
	out_offset = output->pos;
	if ((in_pointer &&
	     !uk_nofault_try_read(in_offset, in_pointer)) ||
	    (out_pointer &&
	     !uk_nofault_try_read(out_offset, out_pointer))) {
		rc = -EFAULT;
		goto out_unlock;
	}
	if (flags) {
		rc = -EINVAL;
		goto out_unlock;
	}
	rc = copy_range_check(input, in_offset, output, out_offset, &count);
	if (rc)
		goto out_unlock;
	copied = copy_range_data(input, in_offset, output, out_offset, count);
	rc = copied;
	if (copied > 0) {
		rc = copy_range_publish_offset(input, in_pointer,
					       in_offset + copied);
		if (copy_range_publish_offset(output, out_pointer,
					      out_offset + copied))
			rc = -EFAULT;
		if (!rc)
			rc = copied;
	}
out_unlock:
	if (second != first)
		uk_mutex_unlock(&second->lock);
	uk_mutex_unlock(&first->lock);
out_output:
	uk_ofile_release(output);
out_input:
	uk_ofile_release(input);
	return rc;
}
