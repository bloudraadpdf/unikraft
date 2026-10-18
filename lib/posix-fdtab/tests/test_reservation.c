/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/errptr.h>
#include <uk/file/nops.h>
#include <uk/posix-fdtab.h>
#include <uk/test.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>
#endif

struct reservation_fixture {
	struct uk_file file;
	uk_file_refcnt references;
	struct uk_file_state state;
	struct uk_ofile *description;
};

static void reservation_file_release(const struct uk_file *file __unused,
				     int what __unused)
{
}

static bool reservation_init(struct reservation_fixture *fixture)
{
	fixture->references = UK_FILE_REFCNT_INIT_VALUE(fixture->references);
	fixture->state = UK_FILE_STATE_INIT_VALUE(fixture->state);
	fixture->file = (struct uk_file){
		.vol = "fdtab-reservation-test",
		.node = fixture,
		.refcnt = &fixture->references,
		.state = &fixture->state,
		.ops = &uk_file_nops,
		._release = reservation_file_release
	};
	fixture->description = uk_ofile_new(&fixture->file, O_RDONLY, 0);
	return fixture->description;
}

static void reservation_finish(struct reservation_fixture *fixture)
{
	if (fixture->description)
		uk_ofile_release(fixture->description);
	uk_file_release(&fixture->file);
}

static int reservation_install(struct reservation_fixture *fixture,
			       unsigned int mode, int *destination)
{
	struct uk_fdtab_reservation *reservation = uk_fdtab_reserve(mode);
	int result;

	if (PTRISERR(reservation))
		return PTR2ERR(reservation);
	uk_ofile_acquire(fixture->description);
	result = uk_fdtab_install(reservation, fixture->description,
				  destination);
	if (result < 0)
		uk_ofile_release(fixture->description);
	return result;
}

UK_TESTCASE(posix_fdtab_reservation, install_copies_number_and_keeps_cloexec)
{
	struct reservation_fixture fixture = {0};
	bool ready = reservation_init(&fixture);

	UK_TEST_EXPECT(ready);
	if (!ready)
		goto out;
	for (unsigned int mode = 0; mode < 2; mode++) {
		for (unsigned int copy = 0; copy < 2; copy++) {
			int number = -1;
			int fd = reservation_install(&fixture,
				mode ? O_CLOEXEC : 0, copy ? &number : NULL);
			struct uk_ofile *installed;

			UK_TEST_EXPECT(fd >= 0);
			if (fd < 0)
				continue;
			UK_TEST_EXPECT(number == (copy ? fd : -1));
			UK_TEST_EXPECT(!!(uk_fdtab_getflags(fd) & O_CLOEXEC) ==
				       !!mode);
			installed = uk_fdtab_get(fd);
			UK_TEST_EXPECT(installed == fixture.description);
			if (installed)
				uk_ofile_release(installed);
			UK_TEST_EXPECT_ZERO(uk_sys_close(fd));
		}
	}
out:
	reservation_finish(&fixture);
}

UK_TESTCASE(posix_fdtab_reservation, fault_keeps_owner_and_releases_slot)
{
	struct reservation_fixture fixture = {0};
	bool ready = reservation_init(&fixture);
	int first, retry, number = -1;

	UK_TEST_EXPECT(ready);
	if (!ready)
		goto out;
	first = reservation_install(&fixture, 0, NULL);
	UK_TEST_EXPECT(first >= 0);
	if (first < 0)
		goto out;
	UK_TEST_EXPECT_ZERO(uk_sys_close(first));
	retry = reservation_install(&fixture, O_CLOEXEC, (int *)1);
	UK_TEST_EXPECT_SNUM_EQ(retry, -EFAULT);
	if (retry >= 0)
		uk_sys_close(retry);
	UK_TEST_EXPECT(uk_refcount_read(&fixture.description->refcnt) == 1);
	retry = reservation_install(&fixture, 0, &number);
	UK_TEST_EXPECT(retry >= 0);
	if (retry >= 0) {
		UK_TEST_EXPECT_SNUM_EQ(retry, first);
		UK_TEST_EXPECT_SNUM_EQ(number, retry);
		UK_TEST_EXPECT_ZERO(uk_fdtab_getflags(retry));
		UK_TEST_EXPECT_ZERO(uk_sys_close(retry));
	}
out:
	reservation_finish(&fixture);
}

#if CONFIG_LIBUKVMEM
UK_TESTCASE(posix_fdtab_reservation, lazy_copyout_completes_before_commit)
{
	struct reservation_fixture fixture = {0};
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	bool ready = reservation_init(&fixture);
	int result, fd;

	UK_TEST_EXPECT(ready);
	if (!ready)
		goto out;
	result = uk_vma_map(uk_vas_get_active(), &address, UK_PAGING_PAGE_SIZE,
			    UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_MAP_PRIVATE,
			    NULL, &uk_vma_anon_ops, NULL, NULL);
	UK_TEST_EXPECT_ZERO(result);
	if (result)
		goto out;
	fd = reservation_install(&fixture, 0, (int *)address);
	UK_TEST_EXPECT(fd >= 0);
	if (fd >= 0) {
		UK_TEST_EXPECT_SNUM_EQ(*(int *)address, fd);
		UK_TEST_EXPECT_ZERO(uk_sys_close(fd));
	}
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(uk_vas_get_active(), address,
					 UK_PAGING_PAGE_SIZE, 0));
out:
	reservation_finish(&fixture);
}
#endif

uk_testsuite_register(posix_fdtab_reservation, NULL);
