/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/sched.h>
#include <uk/plat/config.h>
#include <uk/test.h>
#include <uk/wait.h>
#include <uk/credentials.h>
#include "../user.h"

struct counted_allocator {
	unsigned int live;
	unsigned int calls;
	unsigned int fail_at;
	bool fail;
	struct uk_alloc allocator;
};

static void *counted_malloc(struct uk_alloc *allocator, size_t size)
{
	struct counted_allocator *counted = __containerof(allocator,
		struct counted_allocator, allocator);
	void *allocation;

	counted->calls++;
	if (counted->fail || counted->calls == counted->fail_at)
		return NULL;
	allocation = uk_malloc(uk_alloc_get_default(), size);
	if (allocation)
		counted->live++;
	return allocation;
}

static void counted_free(struct uk_alloc *allocator, void *allocation)
{
	struct counted_allocator *counted = __containerof(allocator,
		struct counted_allocator, allocator);

	counted->live--;
	uk_free(uk_alloc_get_default(), allocation);
}

#define COUNTED_ALLOCATOR_INITIALIZER \
	{ .allocator = { .malloc = counted_malloc, .free = counted_free } }

static void __noreturn unused_thread(void)
{
	UK_CRASH("Unscheduled credential test thread ran\n");
}

static struct uk_thread *create_child(struct uk_alloc *allocator, void *private)
{
	struct uk_alloc *memory = uk_alloc_get_default();

	return uk_thread_create_fn0(allocator, unused_thread, memory, 0,
		NULL, 0, memory, false, "credential-test", private, NULL);
}

static int initialize_child(void)
{
	struct uk_thread child;
	struct uk_alloc *allocator = uk_alloc_get_default();
	int result;

	result = uk_thread_init_fn0(&child, unused_thread, allocator,
				    STACK_SIZE, NULL, 0, allocator, false, NULL,
				    "credential-init", NULL, NULL);
	if (!result)
		uk_thread_release(&child);
	return result;
}

UK_TESTCASE(posix_user_credentials, configured_snapshot)
{
	bool no_new_privs = true;
	struct pu_credentials *credentials =
		pu_credentials_acquire(uk_thread_current(), &no_new_privs);
	const struct pu_identity *identity;
	const struct pu_privileges *privileges;
	__u64 expected = UK_DEFAULT_UID == 0 ? PU_CAP_KNOWN_MASK : 0;

	UK_TEST_EXPECT_NOT_NULL(credentials);
	if (!credentials)
		return;
	identity = pu_credentials_identity(credentials);
	privileges = pu_credentials_privileges(credentials);
	UK_TEST_EXPECT_SNUM_EQ(identity->real_uid, UK_DEFAULT_UID);
	UK_TEST_EXPECT_SNUM_EQ(identity->effective_uid, UK_DEFAULT_UID);
	UK_TEST_EXPECT_SNUM_EQ(identity->saved_uid, UK_DEFAULT_UID);
	UK_TEST_EXPECT_SNUM_EQ(identity->fs_uid, UK_DEFAULT_UID);
	UK_TEST_EXPECT_SNUM_EQ(identity->real_gid, UK_DEFAULT_GID);
	UK_TEST_EXPECT_SNUM_EQ(identity->effective_gid, UK_DEFAULT_GID);
	UK_TEST_EXPECT_SNUM_EQ(identity->saved_gid, UK_DEFAULT_GID);
	UK_TEST_EXPECT_SNUM_EQ(identity->fs_gid, UK_DEFAULT_GID);
	UK_TEST_EXPECT_SNUM_EQ(pu_credentials_group_count(credentials), 0);
	UK_TEST_EXPECT_SNUM_EQ(privileges->effective, expected);
	UK_TEST_EXPECT_SNUM_EQ(privileges->permitted, expected);
	UK_TEST_EXPECT_SNUM_EQ(privileges->bounding, PU_CAP_KNOWN_MASK);
	UK_TEST_EXPECT_SNUM_EQ(privileges->inheritable, 0);
	UK_TEST_EXPECT_SNUM_EQ(privileges->ambient, 0);
	UK_TEST_EXPECT_SNUM_EQ(privileges->securebits, 0);
	UK_TEST_EXPECT(!no_new_privs);
	pu_credentials_release(credentials);
}

UK_TESTCASE(posix_user_credentials, supplementary_groups)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *grouped = NULL, *copy, *empty = NULL;
	const __u32 input[] = {43, 42, 43};
	const __u32 *ids;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	result = pu_credentials_with_groups(&counted.allocator, original, input,
					    3, &grouped);
	if (!(pu_credentials_privileges(original)->effective & PU_CAP_SETGID)) {
		UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
		UK_TEST_EXPECT_NULL(grouped);
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
		goto out;
	}
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!grouped)
		goto out;
	UK_TEST_EXPECT_SNUM_EQ(pu_credentials_group_count(original), 0);
	UK_TEST_EXPECT_SNUM_EQ(pu_credentials_group_count(grouped), 3);
	ids = pu_credentials_group_ids(grouped);
	UK_TEST_EXPECT_SNUM_EQ(ids[0], 42);
	UK_TEST_EXPECT_SNUM_EQ(ids[1], 43);
	UK_TEST_EXPECT_SNUM_EQ(ids[2], 43);
	UK_TEST_EXPECT_SNUM_EQ(input[0], 43);
	UK_TEST_EXPECT(pu_credentials_has_group(grouped, 42));
	UK_TEST_EXPECT(pu_credentials_has_group(grouped, 43));
	UK_TEST_EXPECT(!pu_credentials_has_group(grouped, 0));
	UK_TEST_EXPECT(!pu_credentials_has_group(grouped, 44));
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 2);
	copy = pu_credentials_copy(&counted.allocator, grouped);
	UK_TEST_EXPECT_NOT_NULL(copy);
	pu_credentials_release(grouped);
	if (!copy)
		goto out;
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 2);
	UK_TEST_EXPECT(pu_credentials_has_group(copy, 42));
	result = pu_credentials_with_groups(&counted.allocator, copy, NULL, 0,
					    &empty);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	pu_credentials_release(copy);
	if (empty) {
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
		UK_TEST_EXPECT_SNUM_EQ(pu_credentials_group_count(empty), 0);
		UK_TEST_EXPECT_NULL(pu_credentials_group_ids(empty));
		UK_TEST_EXPECT(!pu_credentials_has_group(empty, 42));
		pu_credentials_release(empty);
	}
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, group_construction_rollback)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *copy = NULL, *denied = NULL;
	const struct pu_privileges *privileges;
	const __u32 group = 42, invalid = UINT32_MAX;
	__u64 effective;
	bool privileged;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	privileged = privileges->effective & PU_CAP_SETGID;
	effective = privileges->effective & ~PU_CAP_SETGID;
	result = pu_credentials_with_groups(&counted.allocator, original, NULL,
					    PU_GROUPS_MAX + 1, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, privileged ? -EINVAL : -EPERM);
	UK_TEST_EXPECT_NULL(copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	if (!privileged)
		goto out;
	for (unsigned int fail = 1; fail <= 2; fail++) {
		counted.calls = 0;
		counted.fail_at = fail;
		result = pu_credentials_with_groups(&counted.allocator,
						    original, &group, 1, &copy);
		UK_TEST_EXPECT_SNUM_EQ(result, -ENOMEM);
		UK_TEST_EXPECT_NULL(copy);
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	}
	counted.fail_at = 0;
	result = pu_credentials_with_groups(&counted.allocator, original,
					    &invalid, 1, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, -EINVAL);
	UK_TEST_EXPECT_NULL(copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	result = pu_credentials_capabilities(&counted.allocator, original,
					     effective,
					     privileges->permitted,
					     privileges->inheritable, &denied);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (denied) {
		result = pu_credentials_with_groups(&counted.allocator,
						    denied, NULL,
						    PU_GROUPS_MAX + 1, &copy);
		UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
		UK_TEST_EXPECT_NULL(copy);
		pu_credentials_release(denied);
	}
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

static bool identity_matches(const struct pu_credentials *credentials,
			     enum pu_id_kind kind, __u32 real, __u32 effective,
			     __u32 saved, __u32 filesystem)
{
	const struct pu_identity *id = pu_credentials_identity(credentials);

	if (kind == PU_ID_USER)
		return id->real_uid == real && id->effective_uid == effective &&
		       id->saved_uid == saved && id->fs_uid == filesystem;
	return id->real_gid == real && id->effective_gid == effective &&
	       id->saved_gid == saved && id->fs_gid == filesystem;
}

UK_TESTCASE(posix_user_credentials, identity_saved_and_filesystem)
{
	const struct id_transition {
		struct pu_id_change change;
		__u32 effective, saved, filesystem;
		int error;
		bool same;
	} transitions[] = {
	    {{PU_ID_FS, .value.id = 44}, 43, 44, 44, 0, false},
	    {{PU_ID_RES, .value.res = {UINT32_MAX, UINT32_MAX, UINT32_MAX}},
	     43,
	     44,
	     44,
	     0,
	     true},
	    {{PU_ID_RE, .value.re = {UINT32_MAX, UINT32_MAX}},
	     43,
	     44,
	     43,
	     0,
	     false},
	    {{PU_ID_FS, .value.id = 44}, 43, 44, 44, 0, false},
	    {{PU_ID_RES, .value.res = {UINT32_MAX, 43, UINT32_MAX}},
	     43,
	     44,
	     43,
	     0,
	     false},
	    {{PU_ID_RE, .value.re = {44, UINT32_MAX}},
	     43,
	     44,
	     43,
	     -EPERM,
	     false},
	    {{PU_ID_RE, .value.re = {UINT32_MAX, 42}}, 42, 44, 42, 0, false},
	    {{PU_ID_RE, .value.re = {42, UINT32_MAX}}, 42, 42, 42, 0, false},
	    {{PU_ID_RES, .value.res = {UINT32_MAX, 44, UINT32_MAX}},
	     42,
	     42,
	     42,
	     -EPERM,
	     false},
	    {{PU_ID_SET, .value.id = 42}, 42, 42, 42, 0, false},
	    {{PU_ID_FS, .value.id = 99}, 42, 42, 42, -EPERM, false},
	    {{PU_ID_SET, .value.id = UINT32_MAX}, 42, 42, 42, -EINVAL, false}};
	struct pu_id_change initial = {PU_ID_RES, .value.res = {42, 43, 44}};
	struct pu_credentials *original, *seed, *active, *next, *current;
	const struct pu_privileges *privileges;
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	for (enum pu_id_kind kind = PU_ID_USER; kind <= PU_ID_GROUP; kind++) {
		__u64 required =
		    kind == PU_ID_USER ? PU_CAP_SETUID : PU_CAP_SETGID;

		result = pu_credentials_with_ids(&counted.allocator, original,
						 kind, &initial, &seed);
		if (!(privileges->effective & required)) {
			UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
			UK_TEST_EXPECT_NULL(seed);
			continue;
		}
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		if (!seed)
			continue;
		const struct pu_privileges *source =
		    pu_credentials_privileges(seed);

		__u64 effective = source->effective & ~required;

		result = pu_credentials_capabilities(&counted.allocator,
						     seed, effective,
						     source->permitted,
						     source->inheritable,
						     &active);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		pu_credentials_release(seed);
		if (!active)
			continue;
		for (size_t i = 0;
		     i < sizeof(transitions) / sizeof(*transitions); i++) {
			const struct id_transition *step = &transitions[i];

			result = pu_credentials_with_ids(&counted.allocator,
							 active, kind,
							 &step->change,
							 &next);
			UK_TEST_EXPECT_SNUM_EQ(result, step->error);
			if (step->error) {
				UK_TEST_EXPECT_NULL(next);
				if (next)
					pu_credentials_release(next);
			} else if (next) {
				if (step->same)
					UK_TEST_EXPECT(next == active);
				pu_credentials_release(active);
				active = next;
			} else {
				UK_TEST_EXPECT_NOT_NULL(next);
			}
			bool matches = identity_matches(active,
							kind, 42,
							step->effective,
							step->saved,
							step->filesystem);
			UK_TEST_EXPECT(matches);
			UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
		}
		pu_credentials_release(active);
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	}
	current = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT(current == original);
	pu_credentials_release(current);
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, identity_capability_fixups)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *active = NULL, *next;
	const struct pu_privileges *privileges, *updated;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	if (!(privileges->effective & PU_CAP_SETUID))
		goto out;
	const __u64 effective = privileges->effective;
	const __u64 permitted = privileges->permitted;
	const struct {
		struct pu_id_change change;
		__u64 effective, permitted;
		bool from_initial, dropped;
	} transitions[] = {
	    {{PU_ID_RES, .value.res = {0, 42, 0}}, 0, permitted, true, false},
	    {{PU_ID_RES, .value.res = {0, 0, 0}},
	     permitted,
	     permitted,
	     false,
	     false},
	    {{PU_ID_FS, .value.id = 42},
	     effective & ~PU_CAP_FS_SET,
	     permitted,
	     true,
	     false},
	    {{PU_ID_FS, .value.id = 0},
	     (effective & ~PU_CAP_FS_SET) | (permitted & PU_CAP_FS_SET),
	     permitted,
	     false,
	     false},
	    {{PU_ID_SET, .value.id = 42}, 0, 0, true, true}};

	for (size_t i = 0; i < sizeof(transitions) / sizeof(*transitions);
	     i++) {
		struct pu_credentials *source =
		    transitions[i].from_initial ? original : active;

		result = pu_credentials_with_ids(&counted.allocator, source,
						 PU_ID_USER,
						 &transitions[i].change, &next);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		if (!next)
			break;
		updated = pu_credentials_privileges(next);
		UK_TEST_EXPECT_SNUM_EQ(updated->effective,
				       transitions[i].effective);
		UK_TEST_EXPECT_SNUM_EQ(updated->permitted,
				       transitions[i].permitted);
		if (transitions[i].dropped) {
			bool matches =
			    identity_matches(next, PU_ID_USER, 42, 42, 42, 42);

			UK_TEST_EXPECT(matches);
			UK_TEST_EXPECT_SNUM_EQ(updated->ambient, 0);
		}
		if (active)
			pu_credentials_release(active);
		active = next;
	}
	if (active)
		pu_credentials_release(active);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, identity_rejection_and_allocation)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *denied = NULL, *next = NULL;
	const struct pu_privileges *privileges;
	struct pu_id_change change = {
	    PU_ID_RES, .value.res = {UINT32_MAX, UINT32_MAX, UINT32_MAX}};
	const __u32 foreign = UK_DEFAULT_UID ? 0 : 1;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	counted.fail = true;
	result = pu_credentials_with_ids(&counted.allocator, original,
					 PU_ID_USER, &change, &next);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT(next == original);
	UK_TEST_EXPECT_SNUM_EQ(counted.calls, 0);
	if (next)
		pu_credentials_release(next);
	counted.fail = false;
	privileges = pu_credentials_privileges(original);
	__u64 effective = privileges->effective & ~PU_CAP_SETUID;

	result = pu_credentials_capabilities(&counted.allocator,
					     original, effective,
					     privileges->permitted,
					     privileges->inheritable, &denied);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!denied)
		goto out;
	counted.fail = true;
	counted.calls = 0;
	change = (struct pu_id_change){
	    PU_ID_RES, .value.res = {UINT32_MAX, foreign, UINT32_MAX}};
	result = pu_credentials_with_ids(&counted.allocator, denied, PU_ID_USER,
					 &change, &next);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	UK_TEST_EXPECT_NULL(next);
	UK_TEST_EXPECT_SNUM_EQ(counted.calls, 0);
	change = (struct pu_id_change){PU_ID_SET, .value.id = foreign};
	result = pu_credentials_with_ids(&counted.allocator, denied, PU_ID_USER,
					 &change, &next);
	UK_TEST_EXPECT_SNUM_EQ(result, -ENOMEM);
	UK_TEST_EXPECT_NULL(next);
	UK_TEST_EXPECT_SNUM_EQ(counted.calls, 1);
	change.value.id = UINT32_MAX;
	result = pu_credentials_with_ids(&counted.allocator, denied, PU_ID_USER,
					 &change, &next);
	UK_TEST_EXPECT_SNUM_EQ(result, -EINVAL);
	UK_TEST_EXPECT_NULL(next);
	UK_TEST_EXPECT_SNUM_EQ(counted.calls, 1);
	pu_credentials_release(denied);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, group_child_retention)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *grouped = NULL, *old, *inherited;
	struct uk_thread *child;
	const __u32 group = 42;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	result = pu_credentials_with_groups(&counted.allocator, original,
					    &group, 1, &grouped);
	if (!(pu_credentials_privileges(original)->effective & PU_CAP_SETGID)) {
		UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
		goto out;
	}
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!grouped)
		goto out;
	old = pu_credentials_exchange(grouped);
	child = create_child(uk_alloc_get_default(), NULL);
	grouped = pu_credentials_exchange(old);
	pu_credentials_release(grouped);
	UK_TEST_EXPECT_NOT_NULL(child);
	if (child) {
		inherited = pu_credentials_acquire(child, NULL);
		UK_TEST_EXPECT_NOT_NULL(inherited);
		if (inherited) {
			UK_TEST_EXPECT(pu_credentials_has_group(inherited, 42));
			UK_TEST_EXPECT_SNUM_EQ(counted.live, 2);
			pu_credentials_release(inherited);
		}
		uk_thread_release(child);
	}
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	inherited = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT(inherited == original);
	pu_credentials_release(inherited);
out:
	pu_credentials_release(original);
}

static bool apply_privilege(struct counted_allocator *counted,
			    struct pu_credentials **active,
			    const struct pu_privilege_change *change,
			    int expected)
{
	struct pu_credentials *next = NULL;
	int result;
	bool matches;

	result = pu_credentials_with_privileges(&counted->allocator, *active,
						change, &next);
	matches = result == expected && !!next == !expected;
	if (next) {
		if (result) {
			pu_credentials_release(next);
		} else {
			pu_credentials_release(*active);
			*active = next;
		}
	}
	return matches;
}

UK_TESTCASE(posix_user_credentials, unprivileged_securebits)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *active = NULL;
	const struct pu_privileges *privileges;
	const unsigned int bits = PU_SECURE_UNPRIVILEGED;
	const unsigned int locked = bits | (bits << 1);
	const struct {
		struct pu_privilege_change change;
		int error;
		unsigned int securebits;
	} transitions[] = {
	    {{PU_PRIV_SECUREBITS, 0}, -EPERM, 0},
	    {{PU_PRIV_SECUREBITS, bits}, 0, bits},
	    {{PU_PRIV_SECUREBITS, 0}, 0, 0},
	    {{PU_PRIV_SECUREBITS, locked}, 0, locked},
	    {{PU_PRIV_SECUREBITS, locked}, -EPERM, locked},
	    {{PU_PRIV_SECUREBITS, bits}, -EPERM, locked},
	    {{PU_PRIV_SECUREBITS, locked | PU_SECURE_KEEP_CAPS},
	     -EPERM,
	     locked},
	    {{PU_PRIV_KEEPCAPS, 1}, 0, locked | PU_SECURE_KEEP_CAPS},
	    {{PU_PRIV_SECUREBITS, locked},
	     -EPERM,
	     locked | PU_SECURE_KEEP_CAPS},
	    {{PU_PRIV_KEEPCAPS, 0}, 0, locked},
	    {{PU_PRIV_KEEPCAPS, 2}, -EINVAL, locked},
	    {{PU_PRIV_BOUNDING_DROP, UINT64_MAX}, -EPERM, locked}};
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	__u64 effective = privileges->effective & ~PU_CAP_SETPCAP;

	result = pu_credentials_capabilities(&counted.allocator, original,
					     effective, privileges->permitted,
					     privileges->inheritable, &active);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!active)
		goto out;
	for (size_t i = 0; i < ARRAY_SIZE(transitions); i++) {
		UK_TEST_EXPECT(apply_privilege(&counted, &active,
					       &transitions[i].change,
					       transitions[i].error));
		const struct pu_privileges *updated =
			pu_credentials_privileges(active);

		UK_TEST_EXPECT_SNUM_EQ(updated->securebits,
				       transitions[i].securebits);
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
	}
	pu_credentials_release(active);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	UK_TEST_EXPECT_SNUM_EQ(privileges->securebits, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, privilege_constructor_errors)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *next = NULL;
	const struct pu_privileges *privileges;
	const struct pu_privilege_change invalid[] = {
	    {PU_PRIV_SECUREBITS, UINT64_C(1) << 63},
	    {PU_PRIV_KEEPCAPS, 2},
	    {PU_PRIV_BOUNDING_DROP, UINT64_MAX},
	    {PU_PRIV_AMBIENT_RAISE, 0},
	    {PU_PRIV_AMBIENT_LOWER, UINT64_MAX},
	    {PU_PRIV_AMBIENT_CLEAR, 1}};
	const int errors[] = {-EPERM, -EINVAL, -EINVAL,
			      -EPERM, -EINVAL, -EINVAL};
	struct pu_privilege_change valid = {PU_PRIV_AMBIENT_CLEAR, 0};
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	counted.fail = true;
	for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
		int expected = errors[i];

		if (invalid[i].operation == PU_PRIV_BOUNDING_DROP &&
		    !(privileges->effective & PU_CAP_SETPCAP))
			expected = -EPERM;
		result = pu_credentials_with_privileges(&counted.allocator,
							original, &invalid[i],
							&next);
		UK_TEST_EXPECT_SNUM_EQ(result, expected);
		UK_TEST_EXPECT_NULL(next);
		UK_TEST_EXPECT_SNUM_EQ(counted.calls, 0);
	}
	result = pu_credentials_with_privileges(&counted.allocator, original,
						&valid, &next);
	UK_TEST_EXPECT_SNUM_EQ(result, -ENOMEM);
	UK_TEST_EXPECT_NULL(next);
	UK_TEST_EXPECT_SNUM_EQ(counted.calls, 1);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, privilege_uid_fixups)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *prepared = NULL, *changed = NULL;
	const struct pu_privileges *privileges, *updated;
	const struct pu_privilege_change policies[] = {
	    {PU_PRIV_KEEPCAPS, 1},
	    {PU_PRIV_SECUREBITS, PU_SECURE_NO_SETUID_FIXUP}};
	struct pu_id_change uid = {PU_ID_SET, .value.id = 42};
	struct pu_id_change fs = {PU_ID_FS, .value.id = 99};
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	if (!(privileges->effective & PU_CAP_SETUID))
		goto out;
	for (size_t i = 0; i < sizeof(policies) / sizeof(*policies); i++) {
		result = pu_credentials_with_privileges(&counted.allocator,
							original, &policies[i],
							&prepared);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		if (!prepared)
			continue;
		result = pu_credentials_with_ids(&counted.allocator, prepared,
						 PU_ID_USER, &uid, &changed);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		pu_credentials_release(prepared);
		if (!changed)
			continue;
		updated = pu_credentials_privileges(changed);
		UK_TEST_EXPECT_SNUM_EQ(updated->permitted,
				       privileges->permitted);
		UK_TEST_EXPECT_SNUM_EQ(updated->effective,
				       i ? privileges->effective : 0);
		if (i) {
			result =
			    pu_credentials_with_ids(&counted.allocator, changed,
						    PU_ID_USER, &fs, &prepared);
			UK_TEST_EXPECT_SNUM_EQ(result, 0);
			if (prepared) {
				updated = pu_credentials_privileges(prepared);
				UK_TEST_EXPECT_SNUM_EQ(updated->effective,
						       privileges->effective);
				pu_credentials_release(prepared);
			}
		}
		pu_credentials_release(changed);
	}
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, ambient_bounding_and_locks)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *seed = NULL, *active, *next;
	const struct pu_privileges *privileges, *updated;
	const struct {
		struct pu_privilege_change change;
		int error;
		__u64 ambient;
		bool dropped;
	} transitions[] = {
	    {{PU_PRIV_AMBIENT_RAISE, 0}, 0, PU_CAP_CHOWN, false},
	    {{PU_PRIV_SECUREBITS, PU_SECURE_KEEP_CAPS_LOCKED},
	     0,
	     PU_CAP_CHOWN,
	     false},
	    {{PU_PRIV_KEEPCAPS, 2}, -EINVAL, PU_CAP_CHOWN, false},
	    {{PU_PRIV_KEEPCAPS, 0}, -EPERM, PU_CAP_CHOWN, false},
	    {{PU_PRIV_SECUREBITS, 0}, -EPERM, PU_CAP_CHOWN, false},
	    {{PU_PRIV_SECUREBITS, PU_SECURE_KEEP_CAPS_LOCKED},
	     0,
	     PU_CAP_CHOWN,
	     false},
	    {{PU_PRIV_SECUREBITS, PU_SECURE_KEEP_CAPS_LOCKED |
				      PU_SECURE_NO_AMBIENT_RAISE |
				      PU_SECURE_NO_AMBIENT_RAISE_LOCKED},
	     0,
	     PU_CAP_CHOWN,
	     false},
	    {{PU_PRIV_AMBIENT_RAISE, 0}, -EPERM, PU_CAP_CHOWN, false},
	    {{PU_PRIV_AMBIENT_LOWER, 0}, 0, 0, false},
	    {{PU_PRIV_AMBIENT_CLEAR, 0}, 0, 0, false},
	    {{PU_PRIV_BOUNDING_DROP, 0}, 0, 0, true}};
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	privileges = pu_credentials_privileges(original);
	if (!(privileges->effective & PU_CAP_SETPCAP) ||
	    !(privileges->permitted & PU_CAP_CHOWN))
		goto out;
	result = pu_credentials_capabilities(&counted.allocator,
					     original, privileges->effective,
					     privileges->permitted,
					     PU_CAP_CHOWN, &seed);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!seed)
		goto out;
	active = pu_credentials_copy(&counted.allocator, seed);
	UK_TEST_EXPECT_NOT_NULL(active);
	if (!active)
		goto seed_out;
	for (size_t i = 0; i < ARRAY_SIZE(transitions); i++) {
		UK_TEST_EXPECT(apply_privilege(&counted, &active,
					       &transitions[i].change,
					       transitions[i].error));
		updated = pu_credentials_privileges(active);
		__u64 bounding = transitions[i].dropped
				     ? privileges->bounding & ~PU_CAP_CHOWN
				     : privileges->bounding;

		UK_TEST_EXPECT_SNUM_EQ(updated->ambient,
				       transitions[i].ambient);
		UK_TEST_EXPECT_SNUM_EQ(updated->bounding, bounding);
		UK_TEST_EXPECT_SNUM_EQ(updated->permitted,
				       privileges->permitted);
		UK_TEST_EXPECT_SNUM_EQ(updated->inheritable, PU_CAP_CHOWN);
	}
	pu_credentials_release(active);
	struct pu_privilege_change raise = {PU_PRIV_AMBIENT_RAISE, 0};

	result = pu_credentials_with_privileges(&counted.allocator, seed,
						&raise, &active);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (active) {
		result = pu_credentials_capabilities(&counted.allocator,
						     active,
						     privileges->effective,
						     privileges->permitted, 0,
						     &next);
		UK_TEST_EXPECT_SNUM_EQ(result, 0);
		if (next) {
			updated = pu_credentials_privileges(next);
			UK_TEST_EXPECT_SNUM_EQ(updated->ambient, 0);
			pu_credentials_release(next);
		}
		pu_credentials_release(active);
	}
seed_out:
	pu_credentials_release(seed);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, capability_construction)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *copy = NULL;
	const struct pu_privileges *old, *new;
	struct pu_privileges expected;
	const __u64 unknown = UINT64_C(1) << 63;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	old = pu_credentials_privileges(original);
	expected = *old;
	counted.fail = true;
	result = pu_credentials_capabilities(&counted.allocator, original, 1,
					     0, 0, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, -ENOMEM);
	UK_TEST_EXPECT(!copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	counted.fail = false;
	result = pu_credentials_capabilities(&counted.allocator, original, 1,
					     0, 0, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	UK_TEST_EXPECT(!copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	result = pu_credentials_capabilities(&counted.allocator, original,
					     old->effective | unknown,
					     old->permitted | unknown,
					     old->inheritable | unknown, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	UK_TEST_EXPECT_NOT_NULL(copy);
	if (copy) {
		new = pu_credentials_privileges(copy);
		UK_TEST_EXPECT_SNUM_EQ(new->effective, expected.effective);
		UK_TEST_EXPECT_SNUM_EQ(new->permitted, expected.permitted);
		UK_TEST_EXPECT_SNUM_EQ(new->inheritable, expected.inheritable);
		UK_TEST_EXPECT_SNUM_EQ(new->bounding, expected.bounding);
		UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
		pu_credentials_release(copy);
	}
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, capability_permitted_drop)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *dropped = NULL, *copy = NULL;
	const struct pu_privileges *old, *remaining;
	__u64 effective, permitted;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	old = pu_credentials_privileges(original);
	effective = old->effective & ~UINT64_C(1);
	permitted = old->permitted & ~UINT64_C(1);
	if (!(old->permitted & 1))
		goto out;
	result = pu_credentials_capabilities(&counted.allocator, original,
					     effective, permitted,
					     old->inheritable, &dropped);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!dropped)
		goto out;
	result = pu_credentials_capabilities(&counted.allocator, dropped,
					     old->effective, old->permitted,
					     old->inheritable, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	UK_TEST_EXPECT(!copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
	remaining = pu_credentials_privileges(dropped);
	UK_TEST_EXPECT_SNUM_EQ(remaining->permitted, permitted);
	pu_credentials_release(dropped);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, capability_inheritable_admission)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *original, *dropped = NULL, *copy = NULL;
	const struct pu_privileges *old, *remaining;
	__u64 effective, permitted;
	const __u64 setpcap = UINT64_C(1) << 8;
	int result;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	old = pu_credentials_privileges(original);
	effective = old->effective & ~(setpcap | 1);
	permitted = old->permitted & ~UINT64_C(1);
	if ((old->effective & (setpcap | 1)) != (setpcap | 1))
		goto out;
	result = pu_credentials_capabilities(&counted.allocator, original,
					     effective, permitted, 0, &dropped);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!dropped)
		goto out;
	result = pu_credentials_capabilities(&counted.allocator, dropped, 0,
					     permitted, 1, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, -EPERM);
	UK_TEST_EXPECT(!copy);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 1);
	pu_credentials_release(dropped);
	result = pu_credentials_capabilities(&counted.allocator, original,
					     effective | setpcap, permitted, 0,
					     &dropped);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (!dropped)
		goto out;
	result = pu_credentials_capabilities(&counted.allocator, dropped,
					     setpcap, permitted, 1, &copy);
	UK_TEST_EXPECT_SNUM_EQ(result, 0);
	if (copy) {
		remaining = pu_credentials_privileges(copy);
		UK_TEST_EXPECT_SNUM_EQ(remaining->inheritable, 1);
		pu_credentials_release(copy);
	}
	pu_credentials_release(dropped);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
out:
	pu_credentials_release(original);
}

UK_TESTCASE(posix_user_credentials, allocation_failure)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;
	struct pu_credentials *credentials =
		pu_credentials_acquire(uk_thread_current(), NULL);
	struct pu_credentials *copy;
	struct pu_credentials *after;
	struct uk_thread *child;

	counted.fail = true;
	copy = pu_credentials_copy(&counted.allocator, credentials);
	child = create_child(&counted.allocator, NULL);
	UK_TEST_EXPECT_NULL(child);
	after = pu_credentials_acquire(uk_thread_current(), NULL);
	UK_TEST_EXPECT(!copy);
	UK_TEST_EXPECT(after == credentials);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	pu_credentials_release(after);
	pu_credentials_release(credentials);
}

enum creator_mode {
	CREATOR_NORMAL,
	CREATOR_NO_TLS,
	CREATOR_UNPUBLISHED
};

struct creator_test {
	struct pu_credentials *original;
	struct uk_waitq wait;
	enum creator_mode mode;
	int creation_error;
	bool different, inherited, no_auxiliary_stack, done;
	bool rejected, parent_unchanged;
	bool nnp_set, nnp_inherited, nnp_rollback, parent_nnp_unchanged;
	bool missing_nnp_unchanged;
	int nnp_error;
	struct counted_allocator counted;
};

static void creator_collected(struct uk_thread *thread __unused, void *argument)
{
	struct creator_test *test = argument;

	__atomic_store_n(&test->done, true, __ATOMIC_RELEASE);
	uk_waitq_wake_up(&test->wait);
}

static bool nnp_rollback_seen;

static int nnp_initializer_fail(struct uk_thread *child,
				struct uk_thread *parent __unused)
{
	struct pu_credentials *credentials;
	bool no_new_privs = false;

	if (child->priv != &nnp_rollback_seen)
		return 0;
	credentials = pu_credentials_acquire(child, &no_new_privs);
	nnp_rollback_seen = credentials && no_new_privs;
	if (credentials)
		pu_credentials_release(credentials);
	return -EAGAIN;
}

UK_THREAD_INIT_PRIO_FLAGS(nnp_initializer_fail, 0, 8,
			  UK_THREAD_INITF_UKTLS);

static void __noreturn creator_thread(void *argument)
{
	struct creator_test *test = argument;
	struct pu_credentials *credentials, *copy, *old;
	struct uk_thread *child;
	struct uk_thread *thread = uk_thread_current();
	bool no_new_privs;

	if (test->mode != CREATOR_NORMAL) {
		struct pu_credentials *detached = NULL;

		if (test->mode == CREATOR_UNPUBLISHED)
			detached = pu_credentials_exchange(NULL);
		test->nnp_error = pu_no_new_privs_set();
		if (test->mode == CREATOR_UNPUBLISHED) {
			no_new_privs = true;
			credentials = pu_credentials_acquire(thread,
							     &no_new_privs);
			test->missing_nnp_unchanged =
				!credentials && no_new_privs;
			if (credentials)
				pu_credentials_release(credentials);
		} else {
			test->missing_nnp_unchanged = true;
		}
		test->creation_error = initialize_child();
		child = create_child(uk_alloc_get_default(), NULL);
		test->rejected = !child;
		if (child)
			uk_thread_release(child);
		if (detached) {
			old = pu_credentials_exchange(detached);
			if (old) {
				test->rejected = false;
				pu_credentials_release(old);
			}
		}
		uk_sched_thread_exit2(creator_collected, test);
	}
	credentials = pu_credentials_acquire(thread, NULL);
	copy = pu_credentials_copy(&test->counted.allocator, credentials);
	pu_credentials_release(credentials);
	if (!copy)
		uk_sched_thread_exit2(creator_collected, test);
	old = pu_credentials_exchange(copy);
	pu_credentials_release(old);
	test->different = copy != test->original;
	test->nnp_error = pu_no_new_privs_set();
	no_new_privs = false;
	credentials = pu_credentials_acquire(thread, &no_new_privs);
	test->nnp_set = !test->nnp_error && no_new_privs &&
		credentials == copy && !pu_no_new_privs_set();
	pu_credentials_release(credentials);
	child = create_child(uk_alloc_get_default(), NULL);
	if (child) {
		test->no_auxiliary_stack = !child->auxsp &&
			!(child->flags & UK_THREADF_AUXSP);
		credentials = pu_credentials_acquire(child, &no_new_privs);
		test->inherited = credentials == copy;
		test->nnp_inherited = no_new_privs;
		pu_credentials_release(credentials);
		uk_thread_release(child);
	}
	nnp_rollback_seen = false;
	child = create_child(uk_alloc_get_default(), &nnp_rollback_seen);
	test->nnp_rollback = !child && nnp_rollback_seen &&
		test->counted.live == 1;
	if (child)
		uk_thread_release(child);
	uk_sched_thread_exit2(creator_collected, test);
}

static bool run_creator(struct creator_test *test, enum creator_mode mode)
{
	struct uk_thread *creator;
	struct uk_thread *thread = uk_thread_current();
	struct uk_sched *scheduler = thread->sched;
	struct pu_credentials *after;
	unsigned long flags;
	bool before_nnp = true, after_nnp = true;

	uk_waitq_init(&test->wait);
	test->mode = mode;
	test->original = pu_credentials_acquire(thread, &before_nnp);
	creator = uk_sched_thread_create_fn1(scheduler, creator_thread, test,
					     0, 0, mode == CREATOR_NO_TLS,
					     false,
					     "credential-creator", NULL, NULL);
	if (creator) {
		uk_waitq_wait_event(&test->wait,
				    __atomic_load_n(&test->done,
						    __ATOMIC_ACQUIRE));
		_uk_waitq_lock(&test->wait, flags);
		_uk_waitq_unlock(&test->wait, flags);
	}
	after = pu_credentials_acquire(thread, &after_nnp);
	test->parent_unchanged = after == test->original;
	test->parent_nnp_unchanged = !before_nnp && !after_nnp;
	pu_credentials_release(after);
	pu_credentials_release(test->original);
	return creator;
}

UK_TESTCASE(posix_user_credentials, actual_creator_inheritance)
{
	struct creator_test test = {
		.counted = COUNTED_ALLOCATOR_INITIALIZER
	};

	UK_TEST_EXPECT(run_creator(&test, CREATOR_NORMAL));
	UK_TEST_EXPECT(test.different);
	UK_TEST_EXPECT(test.inherited);
	UK_TEST_EXPECT(test.no_auxiliary_stack);
	UK_TEST_EXPECT(test.nnp_set);
	UK_TEST_EXPECT(test.nnp_inherited);
	UK_TEST_EXPECT(test.nnp_rollback);
	UK_TEST_EXPECT(test.parent_nnp_unchanged);
	UK_TEST_EXPECT_SNUM_EQ(test.counted.live, 0);
	UK_TEST_EXPECT(test.parent_unchanged);
}

UK_TESTCASE(posix_user_credentials, invalid_creator_rejected)
{
	for (enum creator_mode mode = CREATOR_NO_TLS;
	     mode <= CREATOR_UNPUBLISHED; ++mode) {
		struct creator_test test = {
			.counted = COUNTED_ALLOCATOR_INITIALIZER
		};

		UK_TEST_EXPECT(run_creator(&test, mode));
		UK_TEST_EXPECT(test.rejected);
		UK_TEST_EXPECT_SNUM_EQ(test.creation_error, -EINVAL);
		UK_TEST_EXPECT_SNUM_EQ(test.nnp_error, -EINVAL);
		UK_TEST_EXPECT(test.missing_nnp_unchanged);
		UK_TEST_EXPECT(test.parent_nnp_unchanged);
		UK_TEST_EXPECT(test.parent_unchanged);
	}
}

static struct {
	struct pu_credentials *held;
	unsigned int acquired, released;
} rollback;

static int rollback_acquire(struct uk_thread *child,
			    struct uk_thread *parent __unused)
{
	if (child->priv == &rollback) {
		rollback.held = pu_credentials_acquire(child, NULL);
		rollback.acquired++;
	}
	return 0;
}

static void rollback_release(struct uk_thread *child)
{
	if (child->priv == &rollback) {
		pu_credentials_release(rollback.held);
		rollback.held = NULL;
		rollback.released++;
	}
}

static int rollback_fail(struct uk_thread *child,
			 struct uk_thread *parent __unused)
{
	return child->priv == &rollback ? -EAGAIN : 0;
}

UK_THREAD_INIT_PRIO_FLAGS(rollback_acquire, rollback_release, 7,
			  UK_THREAD_INITF_UKTLS);
UK_THREAD_INIT_PRIO_FLAGS(rollback_fail, 0, 8, UK_THREAD_INITF_UKTLS);

static unsigned int early_failure;

static int early_fail(struct uk_thread *child,
		      struct uk_thread *parent __unused)
{
	return child->priv == &early_failure ? -EAGAIN : 0;
}

UK_THREAD_INIT_PRIO_FLAGS(early_fail, 0, 1, UK_THREAD_INITF_UKTLS);

static bool rejected_child(struct counted_allocator *counted, void *marker)
{
	struct pu_credentials *original, *copy;
	struct uk_thread *child;

	original = pu_credentials_acquire(uk_thread_current(), NULL);
	copy = pu_credentials_copy(&counted->allocator, original);
	if (!copy) {
		pu_credentials_release(original);
		return false;
	}
	pu_credentials_release(pu_credentials_exchange(copy));
	child = create_child(uk_alloc_get_default(), marker);
	pu_credentials_release(pu_credentials_exchange(original));
	if (child)
		uk_thread_release(child);
	return !child;
}

UK_TESTCASE(posix_user_credentials, early_initializer_rollback)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;

	UK_TEST_EXPECT(rejected_child(&counted, &early_failure));
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
}

UK_TESTCASE(posix_user_credentials, failed_initializer_rollback)
{
	struct counted_allocator counted = COUNTED_ALLOCATOR_INITIALIZER;

	UK_TEST_EXPECT(rejected_child(&counted, &rollback));
	UK_TEST_EXPECT_SNUM_EQ(rollback.acquired, 1);
	UK_TEST_EXPECT_SNUM_EQ(rollback.released, 1);
	UK_TEST_EXPECT(!rollback.held);
	UK_TEST_EXPECT_SNUM_EQ(counted.live, 0);
	if (rollback.held) {
		pu_credentials_release(rollback.held);
		rollback.held = NULL;
	}
}

uk_testsuite_register(posix_user_credentials, NULL);
