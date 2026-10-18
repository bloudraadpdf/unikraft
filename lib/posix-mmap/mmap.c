/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2023, Unikraft GmbH and The Unikraft Authors.
 * Licensed under the BSD-3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 */

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/mman.h>
#include <errno.h>

#include <uk/config.h>
#include <uk/syscall.h>
#include <uk/essentials.h>
#include <uk/arch/limits.h>
#include <uk/lcpu.h>
#include <uk/posix-mmap.h>
#include <uk/vmem.h>
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
#include <uk/process.h>
#endif

#if CONFIG_LIBPOSIX_FDTAB
#include <uk/posix-fdtab.h>

#if CONFIG_LIBVFSCORE
#include <vfscore/vma.h>
#include <vfscore/file.h>
#endif /* CONFIG_LIBVFSCORE */

#if CONFIG_LIBPOSIX_FDIO
#include <uk/posix-file-vma.h>
#endif /* CONFIG_LIBPOSIX_FDIO */

#endif /* CONFIG_LIBPOSIX_FDTAB */

#ifndef MAP_UNINITIALIZED
#define MAP_UNINITIALIZED 0x4000000
#endif /* !MAP_UNINITIALIZED */

#define VALID_PROT_MASK		(PROT_READ | PROT_WRITE | PROT_EXEC)

static bool page_range(__vaddr_t address, size_t *length)
{
	if (*length > __SZ_MAX - (UK_PAGING_PAGE_SIZE - 1))
		return false;
	*length = UK_PAGING_PAGE_ALIGN_UP(*length);
	return address <= __VADDR_MAX - *length;
}

static inline unsigned long prot_to_attr(int prot)
{
	unsigned long attr = UK_PAGING_PAGE_ATTR_PROT_NONE;

	/* Note that specifying invalid values is not an error for mmap. You
	 * just don't get any meaningful permissions on the area.
	 */

	if (prot & PROT_READ)
		attr |= UK_PAGING_PAGE_ATTR_PROT_READ;
	if (prot & PROT_WRITE)
		attr |= UK_PAGING_PAGE_ATTR_PROT_WRITE;
	if (prot & PROT_EXEC)
		attr |= UK_PAGING_PAGE_ATTR_PROT_EXEC;

	return attr;
}

static int do_mmap(void **addr, size_t len, int prot, int flags, int fd,
		   off_t offset, struct uk_ofile *source)
{
	struct uk_vas *vas = uk_vas_get_active();
	unsigned long vattr = prot_to_attr(prot);
	unsigned long vflags = 0;
	__vaddr_t vaddr;
#if CONFIG_LIBPOSIX_FDTAB
#if CONFIG_LIBVFSCORE
	struct uk_vma_file_args vfscore_vma_args;
#endif /* CONFIG_LIBVFSCORE */
#if CONFIG_LIBPOSIX_FDIO
	struct uk_fdio_vma_args fdio_vma_args = {0};
#endif /* CONFIG_LIBPOSIX_FDIO */
#endif /* CONFIG_LIBPOSIX_FDTAB */
	void *vargs;
	const struct uk_vma_ops *vops;
	unsigned int order, lvl = UK_PAGING_PAGE_LEVEL;
	int rc;

#if !CONFIG_LIBPOSIX_FDIO
	(void)source;
#endif
	if (unlikely(len == 0))
		return -EINVAL;

	/* len will overflow when aligning it to page size */
	if (unlikely(len > __SZ_MAX - (UK_PAGING_PAGE_SIZE - 1)))
		return -ENOMEM;

	if (unlikely(offset < 0 || !UK_PAGING_PAGE_ALIGNED(offset)))
		return -EINVAL;

	if (unlikely((__sz)offset > __OFF_MAX - len))
		return -EOVERFLOW;

	if (*addr) {
		if ((flags & MAP_FIXED) && !(flags & MAP_FIXED_NOREPLACE))
			vflags |= UK_VMA_MAP_REPLACE;

		vaddr = (__vaddr_t)*addr;
	} else {
		/* Linux returns -EPERM if addr is NULL for a fixed mapping */
		if (unlikely(flags & MAP_FIXED))
			return -EPERM;

		if (unlikely(flags & MAP_FIXED_NOREPLACE))
			return -EPERM;

		vaddr = UK_PAGING_VADDR_ANY;
	}

	if (flags & MAP_ANONYMOUS) {
		if ((flags & MAP_TYPE) != MAP_SHARED &&
		    (flags & MAP_TYPE) != MAP_PRIVATE)
			return -EINVAL;

		/* We do not support unrestricted VMAs */
		if (unlikely(flags & MAP_GROWSDOWN))
			return -ENOTSUP;

		if (flags & MAP_UNINITIALIZED)
			vflags |= UK_VMA_MAP_UNINITIALIZED;
		if ((flags & MAP_TYPE) == MAP_SHARED)
			vflags |= UK_VMA_ANON_SHARED;

		if (flags & MAP_HUGETLB) {
#ifdef UK_PAGING_PAGE_LARGE_SHIFT
			order = (flags >> MAP_HUGE_SHIFT) & MAP_HUGE_MASK;
			if (order == 0)
				order = UK_PAGING_PAGE_LARGE_SHIFT;

			lvl = UK_PAGING_PAGE_SHIFT_Lx(order);

			if (unlikely(!UK_PAGING_PAGE_Lx_HAS(lvl)))
				return -EINVAL;

			vflags |= UK_VMA_MAP_SIZE(order);
#else
			return -EINVAL;
#endif /* UK_PAGING_PAGE_LARGE_SHIFT */
		}

		vargs = NULL;
		vops  = &uk_vma_anon_ops;
	} else {
#if CONFIG_LIBPOSIX_FDTAB
		union uk_shim_file sf;
		int kind;

		if ((flags & MAP_TYPE) == MAP_SHARED ||
		    (flags & MAP_TYPE) == MAP_SHARED_VALIDATE)
			vflags |= UK_VMA_FILE_SHARED;
		else if (unlikely((flags & MAP_TYPE) != MAP_PRIVATE))
			return -EINVAL;

		if (unlikely(flags & MAP_GROWSDOWN))
			return -EINVAL;

		if (unlikely(flags & MAP_HUGETLB))
			return -EINVAL;

		/* Not supported yet. Linux returns EOPNOTSUPP here */
		if (unlikely(flags & MAP_SYNC))
			return -EOPNOTSUPP;

		if (unlikely(!UK_PAGING_PAGE_ALIGNED(offset)))
			return -EINVAL;

#if CONFIG_LIBPOSIX_FDIO
		if (source) {
			sf.ofile = source;
			uk_ofile_acquire(source);
			kind = UK_SHIM_OFILE;
		} else {
			kind = uk_fdtab_shim_get(fd, &sf);
		}
#else
		kind = uk_fdtab_shim_get(fd, &sf);
#endif
		switch (kind) {
		case UK_SHIM_LEGACY:
#if CONFIG_LIBVFSCORE
			vfscore_vma_args.fd = fd;
			vfscore_vma_args.offset = offset;
			vargs = &vfscore_vma_args;
			vops = &uk_vma_file_ops;
			fdrop(sf.vfile);
			break;
#else /* !CONFIG_LIBVFSCORE */
			UK_CRASH("Impossible\n");
#endif /* !CONFIG_LIBVFSCORE */
		case UK_SHIM_OFILE:
#if CONFIG_LIBPOSIX_FDIO
			fdio_vma_args.of = sf.ofile;
			fdio_vma_args.offset = offset;
			vargs = &fdio_vma_args;
			vops = &uk_fdio_vma_ops;
			/* We release the ofile after VMA creation */
			break;
#else /* !CONFIG_LIBPOSIX_FDIO */
			return -ENOTSUP;
#endif /* CONFIG_LIBPOSIX_FDTAB */
		default:
			return -EBADF;
		}
#else
		(void)fd; /* silence warning */

		return -ENOTSUP;
#endif /* CONFIG_LIBPOSIX_FDTAB */
	}

	/* Linux will always align len to the selected page size */
	if (len > __SZ_MAX - (UK_PAGING_PAGE_Lx_SIZE(lvl) - 1)) {
		rc = -ENOMEM;
		goto out;
	}
	len = UK_PAGING_PAGE_Lx_ALIGN_UP(len, lvl);
	if (vaddr != UK_PAGING_VADDR_ANY) {
		if (!(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)))
			vaddr = UK_PAGING_PAGE_Lx_ALIGN_DOWN(vaddr, lvl);
		if (vaddr > __VADDR_MAX - len) {
			if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
				rc = -ENOMEM;
				goto out;
			}
			vaddr = UK_PAGING_VADDR_ANY;
		}
	}

	if (flags & MAP_POPULATE)
		vflags |= UK_VMA_MAP_POPULATE;

	/* MAP_LOCKED: Ignored for now */
	/* MAP_NONBLOCKED: Ignored for now */
	/* MAP_NORESERVE : Ignored for now */

	if ((flags & MAP_TYPE) == MAP_PRIVATE)
		vflags |= UK_VMA_MAP_PRIVATE;
	for (;;) {
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
		rc = uk_posix_process_vm_map(vas, &vaddr, len, vattr, vflags,
					     vops, vargs);
#else
		rc = uk_vma_map(vas, &vaddr, len, vattr, vflags, NULL,
				vops, vargs, NULL);
#endif
		if (!rc || (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) ||
		    vaddr == UK_PAGING_VADDR_ANY)
			break;
		vaddr = UK_PAGING_VADDR_ANY;
	}
out:
#if CONFIG_LIBPOSIX_FDTAB && CONFIG_LIBPOSIX_FDIO
	/* Return open file description after creation of VMAs */
	if (vargs == &fdio_vma_args)
		uk_ofile_release(fdio_vma_args.of);
#endif /* CONFIG_LIBPOSIX_FDTAB && CONFIG_LIBPOSIX_FDIO */

	*addr = (void *)vaddr;
	return rc;
}

#if CONFIG_LIBPOSIX_FDIO
int uk_posix_mmap_file(void **address, size_t length, int protection, int flags,
		       struct uk_ofile *file, off_t offset)
{
	if (!file || (flags & MAP_ANONYMOUS))
		return -EINVAL;
	return do_mmap(address, length, protection, flags, -1, offset, file);
}
#endif

UK_SYSCALL_DEFINE(void *, mmap, void *, addr, size_t, len, int, prot,
		  int, flags, int, fd, off_t, offset)
{
	int rc;

	rc = do_mmap(&addr, len, prot, flags, fd, offset, NULL);
	if (unlikely(rc)) {
		errno = -rc;
		return MAP_FAILED;
	}

	return addr;
}

UK_SYSCALL_R_DEFINE(int, munmap, void *, addr, size_t, len)
{
	struct uk_vas *vas = uk_vas_get_active();
	__vaddr_t vaddr = (__vaddr_t)addr;
	int rc;

	if (unlikely(len == 0) || !UK_PAGING_PAGE_ALIGNED(vaddr) ||
	    !page_range(vaddr, &len))
		return -EINVAL;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	rc = uk_posix_process_vm_unmap(vas, vaddr, len);
#else
	rc = uk_vma_unmap(vas, vaddr, len, 0);
#endif
	if (unlikely(rc)) {
		if (rc == -ENOENT)
			return 0;

		return rc;
	}

	return 0;
}

UK_SYSCALL_R_DEFINE(int, mprotect, void *, addr, size_t, len, int, prot)
{
	struct uk_vas *vas = uk_vas_get_active();
	unsigned long vattr = prot_to_attr(prot);
	__vaddr_t vaddr = (__vaddr_t)addr;
	int grows = prot & (PROT_GROWSDOWN | PROT_GROWSUP);
	int rc;

	/* mprotect does not tolerate invalid prot values */
	if (grows == (PROT_GROWSDOWN | PROT_GROWSUP) ||
	    !UK_PAGING_PAGE_ALIGNED(vaddr))
		return -EINVAL;
	if (!len)
		return 0;
	if (unlikely(prot & ~(VALID_PROT_MASK |
			      PROT_GROWSDOWN | PROT_GROWSUP)) ||
	    (grows & PROT_GROWSUP))
		return -EINVAL;
	if (!page_range(vaddr, &len))
		return -ENOMEM;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	rc = uk_posix_process_vm_protect(vas, vaddr,
					 len, vattr,
					 grows & PROT_GROWSDOWN);
#else
	if (grows)
		return -EINVAL;
	rc = uk_vma_set_attr(vas, vaddr, len, vattr,
			     UK_VMA_FLAG_STRICT_VMA_CHECK);
#endif
	if (unlikely(rc)) {
		if (rc == -ENOENT)
			return -ENOMEM;

		return rc;
	}

	return 0;
}

UK_SYSCALL_R_DEFINE(int, madvise, void *, addr, size_t, len, int, advice)
{
	struct uk_vas *vas = uk_vas_get_active();
	unsigned long vadvice = 0;
	__vaddr_t vaddr = (__vaddr_t)addr;
	int rc;

	if (!UK_PAGING_PAGE_ALIGNED(vaddr) || !page_range(vaddr, &len))
		return -EINVAL;
	if (!len)
		return 0;

	switch (advice) {
	case MADV_DONTNEED:
		vadvice |= UK_VMA_ADV_DONTNEED;
		break;
	default:
		/* Just ignore unsupported advices for now. The call to
		 * uk_vma_advise() does not have an effect but will validate
		 * parameters nevertheless and return errors if needed.
		 */
		break;
	}

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	rc = uk_posix_process_vm_advise(vas, vaddr,
					len, vadvice);
#else
	rc = uk_vma_advise(vas, vaddr, len, vadvice,
			   UK_VMA_FLAG_STRICT_VMA_CHECK);
#endif
	if (unlikely(rc)) {
		if (rc == -ENOENT)
			return -ENOMEM;

		return rc;
	}

	return 0;
}

UK_SYSCALL_R_DEFINE(int, msync, void*, addr, size_t, length, int, flags)
{
	if (!UK_PAGING_PAGE_ALIGNED((__vaddr_t)addr) ||
	    (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) ||
	    ((flags & MS_ASYNC) && (flags & MS_SYNC)))
		return -EINVAL;
	if (!page_range((__vaddr_t)addr, &length))
		return -ENOMEM;
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	int rc = uk_posix_process_vm_sync(uk_vas_get_active(), (__vaddr_t)addr,
					 length);
#else
	int rc = uk_vma_sync(uk_vas_get_active(), (__vaddr_t)addr, length);
#endif
	return rc == -ENOENT ? -ENOMEM : rc;
}

UK_SYSCALL_R_DEFINE(int, mlock, const void*, addr, size_t, len)
{
	/* Since swap memory is not supported, this doesn't make sense */
	return 0;
}
