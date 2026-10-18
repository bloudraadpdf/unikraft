/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_FUTEX_H__
#define __UK_FUTEX_H__

#include <stdbool.h>
#include <uk/arch/types.h>
#include <uk/list.h>

struct posix_vm_context;
struct uk_ofile;
struct uk_vma_anon_identity;
struct uk_thread;
struct uk_vma;
struct uk_alloc;
struct posix_thread;
struct uk_futex_call;

enum uk_futex_key_kind {
	UK_FUTEX_KEY_PRIVATE,
	UK_FUTEX_KEY_VM,
	UK_FUTEX_KEY_FILE,
	UK_FUTEX_KEY_ANON,
};

struct uk_futex_key {
	enum uk_futex_key_kind kind;
	const void *identity;
	__u64 offset;
	struct posix_vm_context *vm;
	struct uk_ofile *file;
	struct uk_vma_anon_identity *anon;
};

static inline bool uk_futex_key_equal(const struct uk_futex_key *first,
				      const struct uk_futex_key *second)
{
	return first->kind == second->kind && first->identity == second->identity &&
	       first->offset == second->offset;
}

int uk_futex_key_get(struct uk_thread *thread, __u32 *address,
		     bool private_key, bool write, struct uk_futex_key *key);
void uk_futex_key_put(struct uk_futex_key *key);
int uk_futex_key_recheck(__u32 *address, bool write,
			 const struct uk_futex_key *key);
int uk_futex_key_mapping(__u32 *address, bool write, struct uk_futex_key *key);
int uk_futex_key_fault(__u32 *address, bool write,
		       const struct uk_futex_key *key);

struct posix_vm_context *uk_posix_futex_vm_get(struct uk_thread *thread);
bool uk_posix_futex_mapping_owned(const struct uk_vma *vma,
				  struct posix_vm_context *state);

struct uk_futex_waiter {
	struct uk_list_head link;
	struct uk_list_head pi_link;
	struct uk_thread *thread;
	struct posix_thread *task;
	struct posix_thread *owner;
	struct uk_futex_call *key_owner;
	const struct uk_futex_key *key;
	__u32 *address;
	int result;
	unsigned int queue_priority;
	bool is_pi;
};

struct uk_futex_call {
	struct uk_futex_call *active_next;
	struct uk_futex_call *retired_next;
	struct uk_alloc *allocator;
	__atomic references;
	struct uk_futex_key keys[2];
	struct uk_futex_waiter waiter;
};

struct uk_futex_call *uk_futex_call_create(struct uk_thread *thread,
					   struct uk_alloc *allocator);
void uk_futex_call_get(struct uk_futex_call *call);
void uk_futex_call_put(struct uk_futex_call *call,
		       struct uk_futex_call **retired);
void uk_futex_call_end(struct uk_thread *thread, struct uk_futex_call *call);
void uk_futex_call_abort_all(struct uk_thread *thread);
void uk_futex_waiter_key_set(struct uk_futex_waiter *waiter,
			     struct uk_futex_call *call, unsigned int index,
			     struct uk_futex_call **retired);
void uk_futex_retired_release(struct uk_futex_call *retired);

bool uk_futex_pi_conflict(const struct uk_futex_key *key);
void uk_futex_queue_add(struct uk_futex_waiter *waiter);
void uk_futex_queue_remove(struct uk_futex_waiter *waiter);
int uk_futex_wake(struct uk_thread *thread, __u32 *address, __u32 maximum,
		  bool private_key);

#endif
