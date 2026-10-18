/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/errptr.h>
#include <uk/futex.h>
#include <uk/sched.h>
#include <uk/test.h>
#include <uk/vmem.h>
#include <uk/vmem/vma_ops.h>
#include "../process.h"

static void __noreturn key_thread_entry(void)
{
	uk_thread_exit();
}

static struct uk_thread *key_thread_create(void)
{
	struct uk_alloc *allocator = uk_alloc_get_default();

	return uk_thread_create_fn0(allocator, key_thread_entry, allocator, 0,
				    NULL, 0, allocator, false, "futex-key",
				    NULL, NULL);
}

UK_TESTCASE(posix_futex_key, private_keys_separate_domains_and_keep_vm_alive)
{
	struct uk_alloc *allocator = uk_alloc_get_default();
	struct posix_process process = {0};
	struct posix_thread target = {.process = &process};
	struct uk_thread *thread = key_thread_create();
	struct posix_vm_context *first = pprocess_vm_alloc(allocator);
	struct posix_vm_context *second = pprocess_vm_alloc(allocator);
	struct uk_futex_key private = {0}, ordinary = {0}, other = {0};
	struct uk_futex_key invalid = {0};
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__u32 *word;
	int rc;

	UK_TEST_EXPECT(thread && !PTRISERR(thread) && first && second);
	if (!thread || PTRISERR(thread) || !first || !second)
		goto out;
	uk_thread_uktls_var(thread, pthread_self) = &target;
	process.vm_context = first;
	rc = uk_vma_map(uk_vas_get_active(), &address, UK_PAGING_PAGE_SIZE,
			UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_MAP_PRIVATE, NULL,
			&uk_vma_anon_ops, NULL, first);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		goto out;
	word = (__u32 *)address;
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, true, false,
					     &private));
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, false, false,
					     &ordinary));
	UK_TEST_EXPECT(!uk_futex_key_equal(&private, &ordinary));
	UK_TEST_EXPECT(first->references == first->users + 2);
	process.vm_context = second;
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, true, false,
					     &other));
	UK_TEST_EXPECT(!uk_futex_key_equal(&private, &other));
	UK_TEST_EXPECT_SNUM_EQ(uk_futex_key_get(thread, word, false, false,
						&invalid), -EFAULT);
	UK_TEST_EXPECT_NULL(invalid.vm);
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, NULL, true, false,
					     &invalid));
	uk_futex_key_put(&invalid);
	UK_TEST_EXPECT_SNUM_EQ(uk_futex_key_get(thread, NULL, false, false,
						&invalid), -EFAULT);
	UK_TEST_EXPECT_NULL(invalid.vm);
	UK_TEST_EXPECT_SNUM_EQ(uk_futex_key_get(thread, (__u32 *)1, true,
						false, &invalid), -EINVAL);
	uk_thread_uktls_var(thread, pthread_self) = NULL;
	pprocess_vm_release_user(first, 0);
	UK_TEST_EXPECT_ZERO(first->users);
	UK_TEST_EXPECT(first->references == 2);
	first = NULL;
out:
	uk_futex_key_put(&private);
	uk_futex_key_put(&ordinary);
	uk_futex_key_put(&other);
	if (thread && !PTRISERR(thread)) {
		uk_thread_uktls_var(thread, pthread_self) = NULL;
		uk_thread_release(thread);
	}
	if (first)
		pprocess_vm_release_user(first, 0);
	if (second)
		pprocess_vm_release_user(second, 0);
}

static int key_shared_anon_map(struct uk_vas *vas, __vaddr_t *address,
			       struct posix_vm_context *state)
{
	return uk_vma_map(vas, address, 2 * UK_PAGING_PAGE_SIZE,
			  UK_PAGING_PAGE_ATTR_PROT_RW, UK_VMA_ANON_SHARED, NULL,
			  &uk_vma_anon_ops, NULL, state);
}

UK_TESTCASE(posix_futex_key, shared_anon_keys_survive_split_unmap_and_replacement)
{
	struct posix_process process = {0};
	struct posix_thread target = {.process = &process};
	struct uk_thread *thread = key_thread_create();
	struct posix_vm_context *state =
		pprocess_vm_alloc(uk_alloc_get_default());
	struct uk_vas *vas = uk_vas_get_active();
	struct uk_futex_key first = {0}, split = {0}, replacement = {0};
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	__u32 *word;
	bool mapped = false;
	int rc;

	UK_TEST_EXPECT(thread && !PTRISERR(thread) && state);
	if (!thread || PTRISERR(thread) || !state)
		goto out;
	process.vm_context = state;
	uk_thread_uktls_var(thread, pthread_self) = &target;
	rc = key_shared_anon_map(vas, &address, state);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		goto out;
	mapped = true;
	word = (__u32 *)(address + UK_PAGING_PAGE_SIZE);
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, false, false,
					     &first));
	UK_TEST_EXPECT(first.kind == UK_FUTEX_KEY_ANON);
	UK_TEST_EXPECT(first.offset == UK_PAGING_PAGE_SIZE);
	UK_TEST_EXPECT_ZERO(uk_vma_set_attr(vas, address, UK_PAGING_PAGE_SIZE,
					    UK_PAGING_PAGE_ATTR_PROT_READ, 0));
	UK_TEST_EXPECT_SNUM_EQ(uk_futex_key_get(thread, (__u32 *)address,
						false, false,
						&replacement), -EFAULT);
	UK_TEST_EXPECT_NULL(replacement.anon);
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, false, false,
					     &split));
	UK_TEST_EXPECT(uk_futex_key_equal(&first, &split));
	UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
					 2 * UK_PAGING_PAGE_SIZE, 0));
	mapped = false;
	rc = key_shared_anon_map(vas, &address, state);
	UK_TEST_EXPECT_ZERO(rc);
	if (rc)
		goto out;
	mapped = true;
	UK_TEST_EXPECT_ZERO(uk_futex_key_get(thread, word, false, false,
					     &replacement));
	UK_TEST_EXPECT(!uk_futex_key_equal(&first, &replacement));
	rc = uk_futex_key_recheck(word, true, &first);
	UK_TEST_EXPECT_SNUM_EQ(rc, -EFAULT);
out:
	if (mapped)
		UK_TEST_EXPECT_ZERO(uk_vma_unmap(vas, address,
						 2 * UK_PAGING_PAGE_SIZE, 0));
	uk_futex_key_put(&first);
	uk_futex_key_put(&split);
	uk_futex_key_put(&replacement);
	if (thread && !PTRISERR(thread)) {
		uk_thread_uktls_var(thread, pthread_self) = NULL;
		uk_thread_release(thread);
	}
	if (state)
		pprocess_vm_release_user(state, 0);
}

static int key_bus_fault(struct uk_vma *vma __unused,
			 struct uk_vm_fault *fault __unused)
{
	return -ENXIO;
}

static int key_memory_fault(struct uk_vma *vma __unused,
			    struct uk_vm_fault *fault __unused)
{
	return -ENOMEM;
}

UK_TESTCASE(posix_futex_key, backing_fault_is_efault_and_oom_is_preserved)
{
	struct posix_vm_context *state =
		pprocess_vm_alloc(uk_alloc_get_default());
	struct uk_vas *vas = uk_vas_get_active();
	struct uk_vma_ops ops = uk_vma_anon_ops;
	struct uk_futex_key key = {0};
	__vaddr_t address = UK_PAGING_VADDR_ANY;
	int rc;

	UK_TEST_EXPECT_NOT_NULL(state);
	if (!state)
		return;
	uk_mutex_lock(&pprocess_mutex);
	state->references++;
	uk_mutex_unlock(&pprocess_mutex);
	key.vm = state;
	ops.fault = key_bus_fault;
	rc = uk_vma_map(vas, &address, UK_PAGING_PAGE_SIZE,
			UK_PAGING_PAGE_ATTR_PROT_RW, 0, NULL, &ops, NULL, state);
	UK_TEST_EXPECT_ZERO(rc);
	if (!rc) {
		rc = uk_futex_key_mapping((__u32 *)address, false, &key);
		UK_TEST_EXPECT_SNUM_EQ(rc, -EFAULT);
		uk_mutex_lock(&vas->mutation_lock);
		ops.fault = key_memory_fault;
		uk_mutex_unlock(&vas->mutation_lock);
		rc = uk_futex_key_mapping((__u32 *)address, false, &key);
		UK_TEST_EXPECT_SNUM_EQ(rc, -ENOMEM);
	}
	uk_futex_key_put(&key);
	pprocess_vm_release_user(state, 0);
}

uk_testsuite_register(posix_futex_key, NULL);
