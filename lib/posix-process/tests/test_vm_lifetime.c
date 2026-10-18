/* SPDX-License-Identifier: BSD-3-Clause */
#include <uk/config.h>

#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING && CONFIG_LIBUKVMEM
#include <uk/alloc.h>
#include <uk/test.h>
#include "../process.h"
#include "lifetime_allocator.h"

static struct posix_vm_context *lifetime_vm_create(struct lifetime_allocator *a)
{
	struct posix_vm_context *state =
		uk_zalloc(&a->allocator, sizeof(*state));

	if (!state)
		return NULL;
	state->allocator = &a->allocator;
	state->users = 1;
	state->references = 2;
	state->view.context = state;
#if CONFIG_LIBPOSIX_PROCESS_EXECVE
	state->auxv = uk_malloc(&a->allocator, 16);
	if (!state->auxv) {
		uk_free(&a->allocator, state);
		return NULL;
	}
#endif
	return state;
}

UK_TESTCASE(posix_vm_lifetime,
	    view_close_preserves_the_pending_cleanup_reference)
{
	struct lifetime_allocator a;
	struct pprocess_retired retired = {0};
	struct posix_vm_context *state;
	size_t live;

	lifetime_allocator_init(&a);
	state = lifetime_vm_create(&a);
	UK_TEST_EXPECT_NOT_NULL(state);
	if (!state)
		return;
	live = a.live;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_retire_locked(state, 0, &retired);
	UK_TEST_EXPECT_SNUM_EQ(state->users, 0);
	UK_TEST_EXPECT_SNUM_EQ(state->references, 2);
	UK_TEST_EXPECT_SNUM_EQ(a.live, live);
	uk_mutex_unlock(&pprocess_mutex);
	uk_posix_vm_view_put(&state->view);
	UK_TEST_EXPECT_SNUM_EQ(state->references, 1);
	UK_TEST_EXPECT_SNUM_EQ(a.live, live);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(a.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(a.locked_frees, 0);
}

UK_TESTCASE(posix_vm_lifetime,
	    cleanup_preserves_a_retained_view_until_its_close)
{
	struct lifetime_allocator a;
	struct pprocess_retired retired = {0};
	struct posix_vm_context *state;

	lifetime_allocator_init(&a);
	state = lifetime_vm_create(&a);
	UK_TEST_EXPECT_NOT_NULL(state);
	if (!state)
		return;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_retire_locked(state, 0, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(state->users, 0);
	UK_TEST_EXPECT_SNUM_EQ(state->references, 1);
	UK_TEST_EXPECT_SNUM_EQ(a.live, 1);
	uk_posix_vm_view_put(&state->view);
	UK_TEST_EXPECT_SNUM_EQ(a.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(a.locked_frees, 0);
}

UK_TESTCASE(posix_vm_lifetime, shared_vm_retires_only_after_its_last_user)
{
	struct lifetime_allocator a;
	struct pprocess_retired retired = {0};
	struct posix_vm_context *state;
	size_t live;

	lifetime_allocator_init(&a);
	state = lifetime_vm_create(&a);
	UK_TEST_EXPECT_NOT_NULL(state);
	if (!state)
		return;
	state->users++;
	state->references++;
	live = a.live;
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_retire_locked(state, 0, &retired);
	UK_TEST_EXPECT_SNUM_EQ(state->users, 1);
	UK_TEST_EXPECT_SNUM_EQ(state->references, 2);
	UK_TEST_EXPECT_NULL(retired.vms);
	UK_TEST_EXPECT_SNUM_EQ(a.live, live);
	uk_mutex_unlock(&pprocess_mutex);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(a.live, live);
	uk_mutex_lock(&pprocess_mutex);
	pprocess_vm_retire_locked(state, 0, &retired);
	uk_mutex_unlock(&pprocess_mutex);
	uk_posix_vm_view_put(&state->view);
	pprocess_retired_release(&retired);
	UK_TEST_EXPECT_SNUM_EQ(a.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(a.locked_frees, 0);
}

uk_testsuite_rootfs_prio(posix_vm_lifetime, NULL, UK_PRIO_LATEST);
#endif
