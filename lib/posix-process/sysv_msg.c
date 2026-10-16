/* SPDX-License-Identifier: BSD-3-Clause */
#include <limits.h>
#include <uk/posix-signal-wait.h>
#include "sysv.h"

#define MSG_NOERROR 010000
#define MSG_EXCEPT 020000
#define MSG_COPY 040000
#define MSG_STAT 11
#define MSG_STAT_ANY 13
#define MSGMAX 8192
#define MSGMNB 16384

struct sysv_msqid {
	struct sysv_perm perm;
	long stime, rtime, ctime;
	unsigned long bytes, count, limit;
	int lspid, lrpid;
	unsigned long unused[2];
};

UK_CTASSERT(sizeof(struct sysv_msqid) == 120);

struct sysv_message {
	struct uk_list_head link;
	long type;
	size_t size;
	char data[];
};

struct msg_queue {
	struct sysv_object object;
	struct sysv_msqid status;
	struct uk_list_head messages;
	struct uk_waitq changed;
};

static void msg_destroy(struct sysv_object *object)
{
	struct msg_queue *queue = (struct msg_queue *)object;
	struct sysv_message *message, *next;

	uk_list_for_each_entry_safe(message, next, &queue->messages, link) {
		uk_list_del(&message->link);
		uk_free(uk_alloc_get_default(), message);
	}
	uk_free(uk_alloc_get_default(), queue);
}

UK_LLSYSCALL_R_DEFINE(int, msgget, int, key, int, flags)
{
	struct sysv_object *object;
	struct msg_queue *queue;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_key(SYSV_MSG, key, flags);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	if (object) {
		rc = object->id;
		goto out;
	}
	queue = uk_zalloc(uk_alloc_get_default(), sizeof(*queue));
	if (!queue) {
		rc = -ENOMEM;
		goto out;
	}
	queue->object.destroy = msg_destroy;
	queue->status.limit = MSGMNB;
	queue->status.ctime = sysv_now();
	UK_INIT_LIST_HEAD(&queue->messages);
	uk_waitq_init(&queue->changed);
	rc = sysv_insert(&queue->object, SYSV_MSG, key, flags);
	if (rc < 0)
		msg_destroy(&queue->object);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static int msg_wait(struct msg_queue *queue, struct uk_posix_signal_wait *sw)
{
	struct uk_mutex *mutex = &pprocess_mutex;
	int rc = uk_posix_signal_wait_check(sw);

	if (rc)
		return rc;
	uk_waitq_wait_deadline_unless_locked(&queue->changed,
					     uk_load_n(&sw->interrupted), 0,
					     uk_mutex_lock, uk_mutex_unlock,
					     mutex);
	return uk_posix_signal_wait_check(sw);
}

static int msg_access(struct msg_queue *queue, unsigned int mode)
{
	if (queue->object.removed)
		return -EIDRM;
	return sysv_access(&queue->object, mode);
}

static int msg_send(int id, struct sysv_message *message, int flags)
{
	struct uk_posix_signal_wait sw;
	struct sysv_object *object;
	struct msg_queue *queue;
	struct sysv_msqid *status;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_id(SYSV_MSG, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	queue = (struct msg_queue *)object;
	status = &queue->status;
	object->references++;
	uk_posix_signal_wait_init(&sw, &queue->changed, NULL, 0);
	for (;;) {
		rc = msg_access(queue, 2);
		if (rc)
			break;
		if (status->bytes <= status->limit &&
		    message->size <= status->limit - status->bytes &&
		    status->count < status->limit) {
			uk_list_add_tail(&message->link, &queue->messages);
			status->bytes += message->size;
			status->count++;
			status->stime = sysv_now();
			status->lspid = uk_pprocess_current()->pid;
			uk_waitq_wake_up(&queue->changed);
			break;
		}
		if (flags & SYSV_IPC_NOWAIT) {
			rc = -EAGAIN;
			break;
		}
		rc = msg_wait(queue, &sw);
		if (rc)
			break;
	}
	uk_posix_signal_wait_end(&sw, rc);
	sysv_put(object);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, msgsnd, int, id, const void *, buffer,
		      size_t, size, int, flags)
{
	struct sysv_message *message;
	long type;
	int rc;

	if (id < 0 || size > MSGMAX)
		return -EINVAL;
	if (!uk_nofault_try_read(type, buffer))
		return -EFAULT;
	if (type <= 0)
		return -EINVAL;
	message = uk_malloc(uk_alloc_get_default(), sizeof(*message) + size);
	if (!message)
		return -ENOMEM;
	message->type = type;
	message->size = size;
	const char *data = (const char *)buffer + sizeof(type);

	if (uk_nofault_memcpy(message->data, data, size, 0) != size)
		rc = -EFAULT;
	else
		rc = msg_send(id, message, flags);
	if (rc)
		uk_free(uk_alloc_get_default(), message);
	return rc;
}

static struct sysv_message *msg_select(struct msg_queue *queue, long type,
				       int flags)
{
	struct sysv_message *message, *best = NULL;
	unsigned long index = 0;
	unsigned long maximum = type == LONG_MIN ? LONG_MAX : -type;

	uk_list_for_each_entry(message, &queue->messages, link) {
		if (flags & MSG_COPY) {
			if (index++ == (unsigned long)type)
				return message;
		} else if (!type) {
			return message;
		} else if (type > 0) {
			if (flags & MSG_EXCEPT ? message->type != type :
			    message->type == type)
				return message;
		} else if ((unsigned long)message->type <= maximum &&
			   (!best || message->type < best->type)) {
			best = message;
		}
	}
	return best;
}

static long msg_receive(struct msg_queue *queue, void *buffer, size_t size,
			long type, int flags)
{
	struct sysv_message *message = msg_select(queue, type, flags);
	long rc;

	if (!message)
		return -ENOMSG;
	if (size < message->size && !(flags & MSG_NOERROR))
		return -E2BIG;
	if ((flags & MSG_COPY) && size < message->size)
		return -EINVAL;
	size = MIN(size, message->size);
	if (!(flags & MSG_COPY)) {
		uk_list_del(&message->link);
		queue->status.count--;
		queue->status.bytes -= message->size;
		queue->status.rtime = sysv_now();
		queue->status.lrpid = uk_pprocess_current()->pid;
		uk_waitq_wake_up(&queue->changed);
	}
	rc = size;
	char *data = (char *)buffer + sizeof(type);

	if (!uk_nofault_try_write(message->type, buffer) ||
	    uk_nofault_memcpy(data, message->data, size, 0) != size)
		rc = -EFAULT;
	if (!(flags & MSG_COPY))
		uk_free(uk_alloc_get_default(), message);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(long, msgrcv, int, id, void *, buffer, size_t, size,
		      long, type, int, flags)
{
	struct uk_posix_signal_wait sw;
	struct sysv_object *object;
	struct msg_queue *queue;
	long rc;

	if (id < 0 || size > LONG_MAX)
		return -EINVAL;
	if (flags & MSG_COPY) {
		size_t length = MIN(size, MSGMAX);

		if ((flags & MSG_EXCEPT) || !(flags & SYSV_IPC_NOWAIT))
			return -EINVAL;
		if (uk_nofault_probe_r((__vaddr_t)buffer + sizeof(type),
				       length, 0) != length)
			return -EFAULT;
	}
	uk_mutex_lock(&pprocess_mutex);
	object = sysv_find_id(SYSV_MSG, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	queue = (struct msg_queue *)object;
	object->references++;
	uk_posix_signal_wait_init(&sw, &queue->changed, NULL, 0);
	for (;;) {
		rc = msg_access(queue, 4);
		if (rc)
			break;
		rc = msg_receive(queue, buffer, size, type, flags);
		if (rc != -ENOMSG || (flags & SYSV_IPC_NOWAIT))
			break;
		rc = msg_wait(queue, &sw);
		if (rc)
			break;
	}
	uk_posix_signal_wait_end(&sw, rc);
	sysv_put(object);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, msgctl, int, id, int, command, void *, buffer)
{
	struct sysv_object *object;
	struct msg_queue *queue;
	struct sysv_msqid data;
	int rc;

	command &= ~0x100;
	uk_mutex_lock(&pprocess_mutex);
	object = command == MSG_STAT || command == MSG_STAT_ANY ?
		sysv_find_index(SYSV_MSG, id) : sysv_find_id(SYSV_MSG, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	queue = (struct msg_queue *)object;
	switch (command) {
	case SYSV_IPC_RMID:
		rc = sysv_owner(object);
		if (!rc) {
			uk_waitq_wake_up(&queue->changed);
			sysv_remove(object);
		}
		break;
	case SYSV_IPC_STAT:
	case MSG_STAT:
	case MSG_STAT_ANY:
		rc = command == MSG_STAT_ANY ? 0 : sysv_access(object, 4);
		if (rc)
			break;
		data = queue->status;
		data.perm = object->perm;
		rc = command == SYSV_IPC_STAT ? 0 : object->id;
		if (!uk_nofault_try_write(data, buffer))
			rc = -EFAULT;
		break;
	case SYSV_IPC_SET:
		if (!uk_nofault_try_read(data, buffer)) {
			rc = -EFAULT;
			break;
		}
		if (data.limit > MSGMNB && uk_syscall_r_geteuid()) {
			rc = -EPERM;
			break;
		}
		rc = sysv_set_perm(object, &data.perm);
		if (!rc) {
			queue->status.limit = data.limit;
			queue->status.ctime = sysv_now();
			uk_waitq_wake_up(&queue->changed);
		}
		break;
	default:
		rc = -EINVAL;
	}
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}
