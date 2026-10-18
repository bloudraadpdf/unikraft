/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/credentials.h>
#include <uk/process.h>
#include <uk/syscall.h>
#include <uk/test.h>

UK_SYSCALL_R_PROTO(2, capget);
UK_SYSCALL_R_PROTO(2, capset);

struct capability_header {
	__u32 version;
	__s32 pid;
};

UK_TESTCASE(posix_process_capabilities, negative_pid_before_input)
{
	struct capability_header header = { 0x20080522, -ENOTSUP };
	struct pu_credentials *before, *after;
	long result;

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	UK_TEST_EXPECT_SNUM_EQ(uk_sys_gettid(), -ENOTSUP);
#endif
	before = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT_NOT_NULL(before);
	result = uk_syscall_r_capset((long)&header, 1);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	after = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT(before == after);
	pu_credentials_release(after);
	pu_credentials_release(before);
}

UK_TESTCASE(posix_process_capabilities, version_only_negotiation)
{
	struct capability_header header = { 0, -1 };

	UK_TEST_EXPECT_SNUM_EQ(uk_syscall_r_capget((long)&header, 0), 0);
	UK_TEST_EXPECT_SNUM_EQ(header.version, 0x20080522);
	UK_TEST_EXPECT_SNUM_EQ(header.pid, -1);
	UK_TEST_EXPECT_SNUM_EQ(uk_syscall_r_capget((long)&header, 1), -EINVAL);
	UK_TEST_EXPECT_SNUM_EQ(uk_syscall_r_capset((long)&header, 1), -EPERM);
}

uk_testsuite_rootfs_prio(posix_process_capabilities, NULL, UK_PRIO_LATEST);
