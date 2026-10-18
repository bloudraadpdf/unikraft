/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef POSIX_PROCESS_SYSV_H
#define POSIX_PROCESS_SYSV_H

#include <errno.h>
#include <uk/alloc.h>
#include <uk/errptr.h>
#include <uk/list.h>
#include <uk/mutex.h>
#include <uk/nofault.h>
#include <uk/posix-time.h>
#include <uk/process.h>
#include <uk/syscall.h>
#include <uk/wait.h>
#include "process.h"

#define SYSV_IPC_CREAT 01000
#define SYSV_IPC_EXCL 02000
#define SYSV_IPC_NOWAIT 04000
#define SYSV_IPC_RMID 0
#define SYSV_IPC_SET 1
#define SYSV_IPC_STAT 2
#define SYSV_IPC_INFO 3
#define SYSV_IPC_MAX_IDS 32768

struct sysv_perm {
	int key;
	__u32 uid, gid, cuid, cgid;
	__u32 mode;
	__u16 sequence, padding;
	unsigned long unused[2];
};

UK_CTASSERT(sizeof(struct sysv_perm) == 48);

enum sysv_kind { SYSV_SHM, SYSV_SEM, SYSV_MSG };

struct sysv_object {
	struct uk_list_head link;
	struct sysv_perm perm;
	enum sysv_kind kind;
	int id;
	unsigned int references;
	bool removed;
	void (*destroy)(struct sysv_object *object);
};

struct sysv_object *sysv_find_key(enum sysv_kind kind, int key, int flags);
struct sysv_object *sysv_find_id(enum sysv_kind kind, int id);
struct sysv_object *sysv_find_index(enum sysv_kind kind, int index);
int sysv_insert(struct sysv_object *object, enum sysv_kind kind,
		int key, int flags);
int sysv_table_info_locked(enum sysv_kind kind, unsigned int *count);
void sysv_remove(struct sysv_object *object);
void sysv_put(struct sysv_object *object);
int sysv_access(const struct sysv_object *object, unsigned int mode);
int sysv_owner(const struct sysv_object *object);
int sysv_set_perm(struct sysv_object *object, const struct sysv_perm *perm);
long sysv_now(void);
void pprocess_shm_retire_locked(struct posix_vm_context *state, pid_t pid);

#endif
