/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <uk/init.h>
#include <uk/refcount.h>
#include <uk/spinlock.h>
#include <uk/credentials.h>
#include "user.h"

UK_CTASSERT(CONFIG_LIBPOSIX_USER_UID >= 0);
UK_CTASSERT(CONFIG_LIBPOSIX_USER_UID < UINT32_MAX);
UK_CTASSERT(CONFIG_LIBPOSIX_USER_GID >= 0);
UK_CTASSERT(CONFIG_LIBPOSIX_USER_GID < UINT32_MAX);

struct pu_groups {
	__atomic references;
	struct uk_alloc *allocator;
	size_t count;
	__u32 ids[];
};

struct pu_credentials {
	__atomic references;
	struct uk_alloc *allocator;
	struct pu_identity identity;
	struct pu_privileges privileges;
	struct pu_groups *groups;
};

static struct pu_credentials initial_credentials = {
	.references = UK_REFCOUNT_INITIALIZER(1),
	.identity = {
		.real_uid = UK_DEFAULT_UID, .effective_uid = UK_DEFAULT_UID,
		.saved_uid = UK_DEFAULT_UID, .fs_uid = UK_DEFAULT_UID,
		.real_gid = UK_DEFAULT_GID, .effective_gid = UK_DEFAULT_GID,
		.saved_gid = UK_DEFAULT_GID, .fs_gid = UK_DEFAULT_GID
	},
	.privileges = {
		.effective = UK_DEFAULT_UID == 0 ? PU_CAP_KNOWN_MASK : 0,
		.permitted = UK_DEFAULT_UID == 0 ? PU_CAP_KNOWN_MASK : 0,
		.bounding = PU_CAP_KNOWN_MASK
	}
};

static __uk_tls struct pu_credentials *current_credentials;
static __uk_tls bool current_no_new_privs;
static struct uk_spinlock publication_lock = UK_SPINLOCK_INITIALIZER();

const struct pu_identity *
pu_credentials_identity(const struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	return &credentials->identity;
}

const struct pu_privileges *
pu_credentials_privileges(const struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	return &credentials->privileges;
}

size_t pu_credentials_group_count(const struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	return credentials->groups ? credentials->groups->count : 0;
}

const __u32 *pu_credentials_group_ids(const struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	return credentials->groups ? credentials->groups->ids : NULL;
}

bool pu_credentials_has_group(const struct pu_credentials *credentials,
			      __u32 group)
{
	const __u32 *ids = pu_credentials_group_ids(credentials);
	size_t lower = 0, upper = pu_credentials_group_count(credentials);

	while (lower < upper) {
		size_t middle = lower + (upper - lower) / 2;

		if (ids[middle] == group)
			return true;
		if (ids[middle] < group)
			lower = middle + 1;
		else
			upper = middle;
	}
	return false;
}

static void groups_release(struct pu_groups *groups)
{
	if (groups && uk_refcount_release(&groups->references))
		uk_free(groups->allocator, groups);
}

struct pu_credentials *pu_credentials_acquire(struct uk_thread *thread,
					      bool *no_new_privs)
{
	struct pu_credentials *credentials;
	unsigned long flags;
	bool nnp;

	UK_ASSERT(thread);
	UK_ASSERT(thread->flags & UK_THREADF_UKTLS);
	uk_spin_lock_irqsave(&publication_lock, flags);
	credentials = uk_thread_uktls_var(thread, current_credentials);
	if (credentials) {
		uk_refcount_acquire(&credentials->references);
		if (no_new_privs) {
			nnp = uk_thread_uktls_var(thread, current_no_new_privs);
			*no_new_privs = nnp;
		}
	}
	uk_spin_unlock_irqrestore(&publication_lock, flags);
	return credentials;
}

struct pu_credentials *pu_credentials_retain(struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	uk_refcount_acquire(&credentials->references);
	return credentials;
}

int pu_no_new_privs_set(void)
{
	struct uk_thread *thread = uk_thread_current();
	unsigned long flags;
	int result = -EINVAL;

	if (!thread || !(thread->flags & UK_THREADF_UKTLS))
		return -EINVAL;
	uk_spin_lock_irqsave(&publication_lock, flags);
	if (current_credentials) {
		current_no_new_privs = true;
		result = 0;
	}
	uk_spin_unlock_irqrestore(&publication_lock, flags);
	return result;
}

void pu_credentials_release(struct pu_credentials *credentials)
{
	UK_ASSERT(credentials);
	if (uk_refcount_release(&credentials->references)) {
		UK_ASSERT(credentials->allocator);
		groups_release(credentials->groups);
		uk_free(credentials->allocator, credentials);
	}
}

struct pu_credentials *
pu_credentials_copy(struct uk_alloc *allocator,
		    const struct pu_credentials *credentials)
{
	struct pu_credentials *copy;

	UK_ASSERT(allocator);
	UK_ASSERT(credentials);
	copy = uk_malloc(allocator, sizeof(*copy));
	if (!copy)
		return NULL;
	uk_refcount_init(&copy->references, 1);
	copy->allocator = allocator;
	copy->identity = credentials->identity;
	copy->privileges = credentials->privileges;
	copy->groups = credentials->groups;
	if (copy->groups)
		uk_refcount_acquire(&copy->groups->references);
	return copy;
}

static int group_compare(const void *left, const void *right)
{
	__u32 a = *(const __u32 *)left, b = *(const __u32 *)right;

	return (a > b) - (a < b);
}

int pu_credentials_with_groups(struct uk_alloc *allocator,
			       const struct pu_credentials *credentials,
			       const __u32 *groups, size_t count,
			       struct pu_credentials **replacement)
{
	struct pu_credentials *copy;
	struct pu_groups *vector = NULL;

	UK_ASSERT(replacement);
	*replacement = NULL;
	if (!(credentials->privileges.effective & PU_CAP_SETGID))
		return -EPERM;
	if (count > PU_GROUPS_MAX)
		return -EINVAL;
	UK_ASSERT(!count || groups);
	copy = pu_credentials_copy(allocator, credentials);
	if (!copy)
		return -ENOMEM;
	if (count) {
		vector = uk_malloc(allocator, sizeof(*vector) +
						  count * sizeof(*vector->ids));
		if (!vector) {
			pu_credentials_release(copy);
			return -ENOMEM;
		}
		uk_refcount_init(&vector->references, 1);
		vector->allocator = allocator;
		vector->count = count;
		memcpy(vector->ids, groups, count * sizeof(*vector->ids));
		for (size_t i = 0; i < count; i++) {
			if (vector->ids[i] == UINT32_MAX) {
				groups_release(vector);
				pu_credentials_release(copy);
				return -EINVAL;
			}
		}
		qsort(vector->ids, count, sizeof(*vector->ids), group_compare);
	}
	groups_release(copy->groups);
	copy->groups = vector;
	*replacement = copy;
	return 0;
}

struct pu_ids {
	__u32 real, effective, saved, filesystem;
};

static bool id_matches(const struct pu_ids *ids, __u32 id)
{
	return id == ids->real || id == ids->effective || id == ids->saved;
}

static bool ids_unchanged(const struct pu_ids *ids,
			  const struct pu_id_change *change)
{
	return (change->value.res.real == UINT32_MAX ||
		change->value.res.real == ids->real) &&
	       (change->value.res.effective == UINT32_MAX ||
		(change->value.res.effective == ids->effective &&
		 change->value.res.effective == ids->filesystem)) &&
	       (change->value.res.saved == UINT32_MAX ||
		change->value.res.saved == ids->saved);
}

static int ids_transform(struct pu_ids *ids, bool privileged,
			 const struct pu_id_change *change)
{
	struct pu_ids old = *ids;
	__u32 real, effective, saved;

	switch (change->operation) {
	case PU_ID_SET:
		if (!privileged && change->value.id != old.real &&
		    change->value.id != old.saved)
			return -EPERM;
		if (privileged) {
			ids->real = change->value.id;
			ids->saved = change->value.id;
		}
		ids->effective = change->value.id;
		ids->filesystem = change->value.id;
		break;
	case PU_ID_RE:
		real = change->value.re.real;
		effective = change->value.re.effective;
		if (!privileged &&
		    ((real != UINT32_MAX && real != old.real &&
		      real != old.effective) ||
		     (effective != UINT32_MAX && !id_matches(&old, effective))))
			return -EPERM;
		if (real != UINT32_MAX)
			ids->real = real;
		if (effective != UINT32_MAX)
			ids->effective = effective;
		if (real != UINT32_MAX ||
		    (effective != UINT32_MAX && effective != old.real))
			ids->saved = ids->effective;
		ids->filesystem = ids->effective;
		break;
	case PU_ID_RES:
		real = change->value.res.real;
		effective = change->value.res.effective;
		saved = change->value.res.saved;
		if (!privileged &&
		    ((real != UINT32_MAX && !id_matches(&old, real)) ||
		     (effective != UINT32_MAX &&
		      !id_matches(&old, effective)) ||
		     (saved != UINT32_MAX && !id_matches(&old, saved))))
			return -EPERM;
		if (real != UINT32_MAX)
			ids->real = real;
		if (effective != UINT32_MAX)
			ids->effective = effective;
		if (saved != UINT32_MAX)
			ids->saved = saved;
		ids->filesystem = ids->effective;
		break;
	case PU_ID_FS:
		if (!privileged && !id_matches(&old, change->value.id) &&
		    change->value.id != old.filesystem)
			return -EPERM;
		ids->filesystem = change->value.id;
		break;
	}
	return 0;
}

static void uid_privileges(struct pu_privileges *privileges,
			   const struct pu_ids *old, const struct pu_ids *ids,
			   enum pu_id_operation operation)
{
	if (privileges->securebits & PU_SECURE_NO_SETUID_FIXUP)
		return;
	if (operation == PU_ID_FS) {
		if (!old->filesystem && ids->filesystem)
			privileges->effective &= ~PU_CAP_FS_SET;
		else if (old->filesystem && !ids->filesystem)
			privileges->effective |=
			    privileges->permitted & PU_CAP_FS_SET;
		return;
	}
	if ((!old->real || !old->effective || !old->saved) && ids->real &&
	    ids->effective && ids->saved) {
		if (!(privileges->securebits & PU_SECURE_KEEP_CAPS)) {
			privileges->effective = 0;
			privileges->permitted = 0;
		}
		privileges->ambient = 0;
	}
	if (!old->effective && ids->effective)
		privileges->effective = 0;
	else if (old->effective && !ids->effective)
		privileges->effective = privileges->permitted;
}

int pu_credentials_with_ids(struct uk_alloc *allocator,
			    struct pu_credentials *credentials,
			    enum pu_id_kind kind,
			    const struct pu_id_change *change,
			    struct pu_credentials **replacement)
{
	const struct pu_identity *identity;
	struct pu_credentials *copy;
	struct pu_ids old, ids;
	__u64 required;
	bool privileged;
	int result;

	UK_ASSERT(allocator);
	UK_ASSERT(credentials);
	UK_ASSERT(change);
	UK_ASSERT(replacement);
	*replacement = NULL;
	identity = &credentials->identity;
	if ((kind != PU_ID_USER && kind != PU_ID_GROUP) ||
	    (unsigned int)change->operation > PU_ID_FS)
		return -EINVAL;
	if ((change->operation == PU_ID_SET || change->operation == PU_ID_FS) &&
	    change->value.id == UINT32_MAX)
		return -EINVAL;
	if (kind == PU_ID_USER) {
		old =
		    (struct pu_ids){identity->real_uid, identity->effective_uid,
				    identity->saved_uid, identity->fs_uid};
		required = PU_CAP_SETUID;
	} else {
		old =
		    (struct pu_ids){identity->real_gid, identity->effective_gid,
				    identity->saved_gid, identity->fs_gid};
		required = PU_CAP_SETGID;
	}
	if (change->operation == PU_ID_RES && ids_unchanged(&old, change)) {
		uk_refcount_acquire(&credentials->references);
		*replacement = credentials;
		return 0;
	}
	ids = old;
	privileged = credentials->privileges.effective & required;
	result = ids_transform(&ids, privileged, change);
	if (change->operation == PU_ID_RES && result)
		return result;
	copy = pu_credentials_copy(allocator, credentials);
	if (!copy)
		return -ENOMEM;
	if (result) {
		pu_credentials_release(copy);
		return result;
	}
	if (kind == PU_ID_USER) {
		copy->identity.real_uid = ids.real;
		copy->identity.effective_uid = ids.effective;
		copy->identity.saved_uid = ids.saved;
		copy->identity.fs_uid = ids.filesystem;
		uid_privileges(&copy->privileges, &old, &ids,
			       change->operation);
	} else {
		copy->identity.real_gid = ids.real;
		copy->identity.effective_gid = ids.effective;
		copy->identity.saved_gid = ids.saved;
		copy->identity.fs_gid = ids.filesystem;
	}
	*replacement = copy;
	return 0;
}

static int privileges_transform(struct pu_privileges *privileges,
				const struct pu_privilege_change *change)
{
	const __u64 value = change->value;
	__u64 locks, changed, unprivileged, capability;
	const __u64 secure_valid = PU_SECURE_ALL_BITS | PU_SECURE_ALL_LOCKS;

	switch (change->operation) {
	case PU_PRIV_SECUREBITS:
		locks = privileges->securebits & PU_SECURE_ALL_LOCKS;
		changed = privileges->securebits ^ value;
		if (((locks >> 1) & changed) || (locks & ~value) ||
		    (value & ~secure_valid))
			return -EPERM;
		unprivileged =
		    PU_SECURE_UNPRIVILEGED | (PU_SECURE_UNPRIVILEGED << 1);
		if (!(privileges->effective & PU_CAP_SETPCAP) &&
		    (!changed || (changed & ~unprivileged)))
			return -EPERM;
		privileges->securebits = (unsigned int)value;
		break;
	case PU_PRIV_KEEPCAPS:
		if (value > 1)
			return -EINVAL;
		if (privileges->securebits & PU_SECURE_KEEP_CAPS_LOCKED)
			return -EPERM;
		if (value)
			privileges->securebits |= PU_SECURE_KEEP_CAPS;
		else
			privileges->securebits &= ~PU_SECURE_KEEP_CAPS;
		break;
	case PU_PRIV_BOUNDING_DROP:
		if (!(privileges->effective & PU_CAP_SETPCAP))
			return -EPERM;
		if (value > PU_CAP_LAST)
			return -EINVAL;
		privileges->bounding &= ~(UINT64_C(1) << value);
		break;
	case PU_PRIV_AMBIENT_RAISE:
	case PU_PRIV_AMBIENT_LOWER:
		if (value > PU_CAP_LAST)
			return -EINVAL;
		capability = UINT64_C(1) << value;
		if (change->operation == PU_PRIV_AMBIENT_RAISE) {
			if (!(privileges->permitted & capability) ||
			    !(privileges->inheritable & capability) ||
			    (privileges->securebits &
			     PU_SECURE_NO_AMBIENT_RAISE))
				return -EPERM;
			privileges->ambient |= capability;
		} else {
			privileges->ambient &= ~capability;
		}
		break;
	case PU_PRIV_AMBIENT_CLEAR:
		if (value)
			return -EINVAL;
		privileges->ambient = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

int pu_credentials_with_privileges(struct uk_alloc *allocator,
				   const struct pu_credentials *credentials,
				   const struct pu_privilege_change *change,
				   struct pu_credentials **replacement)
{
	struct pu_privileges privileges;
	struct pu_credentials *copy;
	int result;

	UK_ASSERT(allocator);
	UK_ASSERT(credentials);
	UK_ASSERT(change);
	UK_ASSERT(replacement);
	*replacement = NULL;
	privileges = credentials->privileges;
	result = privileges_transform(&privileges, change);
	if (result)
		return result;
	copy = pu_credentials_copy(allocator, credentials);
	if (!copy)
		return -ENOMEM;
	copy->privileges = privileges;
	*replacement = copy;
	return 0;
}

int pu_credentials_capabilities(struct uk_alloc *allocator,
				const struct pu_credentials *credentials,
				__u64 effective, __u64 permitted,
				__u64 inheritable,
				struct pu_credentials **replacement)
{
	const struct pu_privileges *old;
	struct pu_credentials *copy;

	UK_ASSERT(replacement);
	*replacement = NULL;
	copy = pu_credentials_copy(allocator, credentials);
	if (!copy)
		return -ENOMEM;
	old = &credentials->privileges;
	effective &= PU_CAP_KNOWN_MASK;
	permitted &= PU_CAP_KNOWN_MASK;
	inheritable &= PU_CAP_KNOWN_MASK;
	if ((permitted & ~old->permitted) || (effective & ~permitted) ||
	    (inheritable & ~(old->inheritable | old->bounding)) ||
	    (!(old->effective & PU_CAP_SETPCAP) &&
	     (inheritable & ~(old->inheritable | old->permitted)))) {
		pu_credentials_release(copy);
		return -EPERM;
	}
	copy->privileges.effective = effective;
	copy->privileges.permitted = permitted;
	copy->privileges.inheritable = inheritable;
	copy->privileges.ambient &= permitted & inheritable;
	*replacement = copy;
	return 0;
}

struct pu_credentials *
pu_credentials_exchange(struct pu_credentials *credentials)
{
	struct pu_credentials *previous;
	unsigned long flags;

	uk_spin_lock_irqsave(&publication_lock, flags);
	previous = current_credentials;
	current_credentials = credentials;
	uk_spin_unlock_irqrestore(&publication_lock, flags);
	return previous;
}

static int credentials_thread_init(struct uk_thread *child,
				   struct uk_thread *parent)
{
	struct pu_credentials *credentials;
	bool no_new_privs = false;

	if (parent) {
		if (!(parent->flags & UK_THREADF_UKTLS))
			return -EINVAL;
		credentials = pu_credentials_acquire(parent, &no_new_privs);
		if (!credentials)
			return -EINVAL;
	} else {
		credentials = &initial_credentials;
		uk_refcount_acquire(&credentials->references);
	}
	uk_thread_uktls_var(child, current_credentials) = credentials;
	uk_thread_uktls_var(child, current_no_new_privs) = no_new_privs;
	return 0;
}

static void credentials_thread_term(struct uk_thread *child __unused)
{
	struct pu_credentials *credentials = pu_credentials_exchange(NULL);

	if (credentials)
		pu_credentials_release(credentials);
}

UK_THREAD_INIT_PRIO_FLAGS(credentials_thread_init, credentials_thread_term,
			  UK_PRIO_EARLIEST, UK_THREAD_INITF_UKTLS);

static int credentials_init(struct uk_init_ctx *context __unused)
{
	if (!current_credentials) {
		uk_refcount_acquire(&initial_credentials.references);
		current_credentials = &initial_credentials;
	}
	return 0;
}

uk_lib_initcall(credentials_init, 0x0);
