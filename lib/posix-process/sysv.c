/* SPDX-License-Identifier: BSD-3-Clause */
#include <limits.h>
#include <unistd.h>
#include "sysv.h"

static UK_LIST_HEAD(objects);
static unsigned int id_counts[3];
static unsigned int next_id[3];

int sysv_access(const struct sysv_object *object, unsigned int mode)
{
	unsigned int allowed = object->perm.mode;
	uid_t uid = (uid_t)uk_syscall_r_geteuid();

	if (!uid)
		return 0;
	if (uid == object->perm.uid || uid == object->perm.cuid)
		allowed >>= 6;
	else if ((gid_t)uk_syscall_r_getegid() == object->perm.gid ||
		 (gid_t)uk_syscall_r_getegid() == object->perm.cgid)
		allowed >>= 3;
	return (allowed & mode) == mode ? 0 : -EACCES;
}

int sysv_owner(const struct sysv_object *object)
{
	uid_t uid = (uid_t)uk_syscall_r_geteuid();

	return !uid || uid == object->perm.uid || uid == object->perm.cuid ?
		0 : -EPERM;
}

struct sysv_object *sysv_find_key(enum sysv_kind kind, int key, int flags)
{
	struct sysv_object *object;
	unsigned int mode = ((flags >> 6) | (flags >> 3) | flags) & 7;
	int rc;

	if (key) {
		uk_list_for_each_entry(object, &objects, link) {
			if (object->kind != kind || object->perm.key != key)
				continue;
			if ((flags & SYSV_IPC_CREAT) && (flags & SYSV_IPC_EXCL))
				return ERR2PTR(-EEXIST);
			rc = sysv_access(object, mode);
			return rc ? ERR2PTR(rc) : object;
		}
	}
	return !key || (flags & SYSV_IPC_CREAT) ? NULL : ERR2PTR(-ENOENT);
}

struct sysv_object *sysv_find_id(enum sysv_kind kind, int id)
{
	struct sysv_object *object;

	uk_list_for_each_entry(object, &objects, link)
		if (object->kind == kind && object->id == id)
			return object;
	return ERR2PTR(-EINVAL);
}

struct sysv_object *sysv_find_index(enum sysv_kind kind, int index)
{
	struct sysv_object *object;

	if (index < 0 || index >= SYSV_IPC_MAX_IDS)
		return ERR2PTR(-EINVAL);
	uk_list_for_each_entry(object, &objects, link)
		if (object->kind == kind &&
		    object->id % SYSV_IPC_MAX_IDS == index)
			return object;
	return ERR2PTR(-EINVAL);
}

int sysv_table_info_locked(enum sysv_kind kind, unsigned int *count)
{
	struct sysv_object *object;
	int highest = 0;

	*count = id_counts[kind];
	uk_list_for_each_entry(object, &objects, link)
		if (object->kind == kind)
			highest = MAX(highest, object->id % SYSV_IPC_MAX_IDS);
	return highest;
}

int sysv_insert(struct sysv_object *object, enum sysv_kind kind,
		int key, int flags)
{
	if (id_counts[kind] == SYSV_IPC_MAX_IDS)
		return -ENOSPC;
	while (next_id[kind] < INT_MAX &&
	       !PTRISERR(sysv_find_index(kind,
					next_id[kind] % SYSV_IPC_MAX_IDS)))
		next_id[kind]++;
	if (next_id[kind] == INT_MAX)
		return -ENOSPC;
	object->kind = kind;
	object->id = next_id[kind]++;
	object->references = 1;
	object->perm = (struct sysv_perm) {
		.key = key,
		.uid = (uid_t)uk_syscall_r_geteuid(),
		.cuid = (uid_t)uk_syscall_r_geteuid(),
		.gid = (gid_t)uk_syscall_r_getegid(),
		.cgid = (gid_t)uk_syscall_r_getegid(),
		.mode = flags & 0777,
		.sequence = object->id / SYSV_IPC_MAX_IDS,
	};
	id_counts[kind]++;
	uk_list_add_tail(&object->link, &objects);
	return object->id;
}

void sysv_put(struct sysv_object *object)
{
	UK_ASSERT(object->references);
	if (!--object->references)
		object->destroy(object);
}

void sysv_remove(struct sysv_object *object)
{
	UK_ASSERT(!object->removed);
	object->removed = true;
	uk_list_del(&object->link);
	id_counts[object->kind]--;
	sysv_put(object);
}

int sysv_set_perm(struct sysv_object *object, const struct sysv_perm *perm)
{
	uid_t uid = (uid_t)uk_syscall_r_geteuid();
	gid_t gid = (gid_t)uk_syscall_r_getegid();
	int rc = sysv_owner(object);

	if (rc)
		return rc;
	if (uid && (perm->uid != object->perm.uid ||
		    (perm->gid != gid && perm->gid != object->perm.gid)))
		return -EPERM;
	object->perm.uid = perm->uid;
	object->perm.gid = perm->gid;
	object->perm.mode = (object->perm.mode & ~0777U) | (perm->mode & 0777);
	return 0;
}

long sysv_now(void)
{
	struct timespec value;
	int rc = uk_sys_clock_gettime(CLOCK_REALTIME, &value);

	UK_ASSERT(!rc);
	return value.tv_sec;
}
