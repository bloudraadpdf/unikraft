/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <limits.h>
#include <uk/alloc.h>
#include <uk/file-lock.h>
#include <uk/mutex.h>
#include <uk/wait.h>
#if CONFIG_LIBUKFS
#include <uk/fs.h>
#endif
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
#include <uk/posix-signal-wait.h>
#endif

struct file_lock {
    struct file_lock *next;
    struct uk_file_lock_request value;
};
struct lock_waiter {
    struct lock_waiter *next;
    const struct uk_file_lock_request *request;
    int reached;
};
static struct uk_mutex lock_mutex = UK_MUTEX_INITIALIZER(lock_mutex);
static struct uk_waitq lock_waitq = UK_WAIT_QUEUE_INITIALIZER(lock_waitq);
static struct file_lock *held;
static struct lock_waiter *waiting;

static int same_file(const struct uk_file *a, const struct uk_file *b)
{
	if (a->ops->getstat == uk_file_anon_getstat &&
	    b->ops->getstat == uk_file_anon_getstat)
		return 1;
#if CONFIG_LIBUKFS
	a = uk_fs_inode(a);
	b = uk_fs_inode(b);
	if (uk_fs_isfs(a) || uk_fs_isfs(b))
		return uk_fs_isfs(a) && uk_fs_isfs(b) && a->state == b->state;
#endif
	return a->vol == b->vol && a->node == b->node;
}

static int same_owner(const struct uk_file_lock_request *a,
		      const struct uk_file_lock_request *b)
{
	return same_file(a->file, b->file) && a->kind == b->kind &&
		a->owner == b->owner;
}

static int overlaps(const struct uk_file_lock_request *a,
                    const struct uk_file_lock_request *b)
{
    return a->start <= b->end && b->start <= a->end;
}

static int conflicts(const struct uk_file_lock_request *a,
		     const struct uk_file_lock_request *b)
{
	return same_file(a->file, b->file) && !same_owner(a, b) &&
		(a->kind == UK_FILE_LOCK_FLOCK) ==
		(b->kind == UK_FILE_LOCK_FLOCK) &&
		(a->type == F_WRLCK || b->type == F_WRLCK) && overlaps(a, b);
}

static struct file_lock *find_conflict(const struct uk_file_lock_request *request)
{
    for (struct file_lock *lock = held; lock; lock = lock->next)
        if (conflicts(request, &lock->value))
            return lock;
    return NULL;
}

static void remove_owner(const struct uk_file *file, const void *owner,
			 enum uk_file_lock_kind kind)
{
	struct file_lock **entry = &held;

	while (*entry) {
		struct file_lock *lock = *entry;

		if (same_file(lock->value.file, file) &&
		    lock->value.owner == owner &&
		    lock->value.kind == kind) {
			*entry = lock->next;
			uk_free(uk_alloc_get_default(), lock);
		} else {
			entry = &lock->next;
		}
	}
	uk_waitq_wake_up(&lock_waitq);
}

void uk_file_lock_release(const struct uk_file *file, const void *owner,
			  enum uk_file_lock_kind kind)
{
	uk_mutex_lock(&lock_mutex);
	remove_owner(file, owner, kind);
	uk_mutex_unlock(&lock_mutex);
}

static int replace_range(const struct uk_file_lock_request *request)
{
    struct uk_alloc *allocator = uk_alloc_get_default();
    struct file_lock *insert = NULL, *tail = NULL;
    if (request->type != F_UNLCK) {
        insert = uk_malloc(allocator, sizeof(*insert));
        if (!insert)
            return -ENOLCK;
        insert->value = *request;
    }
    for (struct file_lock *lock = held; lock; lock = lock->next) {
        if (same_owner(request, &lock->value) &&
            lock->value.start < request->start && lock->value.end > request->end) {
            tail = uk_malloc(allocator, sizeof(*tail));
            if (!tail) {
                uk_free(allocator, insert);
                return -ENOLCK;
            }
            tail->value = lock->value;
            tail->value.start = request->end + 1;
            break;
        }
    }
    struct file_lock **entry = &held;
    while (*entry) {
        struct file_lock *lock = *entry;
        if (!same_owner(request, &lock->value) || !overlaps(request, &lock->value)) {
            entry = &lock->next;
            continue;
        }
        if (lock->value.start < request->start) {
            lock->value.end = request->start - 1;
            entry = &lock->next;
        } else if (lock->value.end > request->end) {
            lock->value.start = request->end + 1;
            entry = &lock->next;
        } else {
            *entry = lock->next;
            uk_free(allocator, lock);
        }
    }
    if (tail) {
        tail->next = held;
        held = tail;
    }
    if (insert) {
        entry = &held;
        while (*entry) {
            struct file_lock *lock = *entry;
            if (same_owner(request, &lock->value) && request->type == lock->value.type &&
                (insert->value.start <= lock->value.end ||
                 (lock->value.end != LLONG_MAX && insert->value.start == lock->value.end + 1)) &&
                (lock->value.start <= insert->value.end ||
                 (insert->value.end != LLONG_MAX && lock->value.start == insert->value.end + 1))) {
                insert->value.start = MIN(insert->value.start, lock->value.start);
                insert->value.end = MAX(insert->value.end, lock->value.end);
                *entry = lock->next;
                uk_free(allocator, lock);
            } else {
                entry = &lock->next;
            }
        }
        insert->next = held;
        held = insert;
    }
    uk_waitq_wake_up(&lock_waitq);
    return 0;
}

static int deadlocked(const struct uk_file_lock_request *request)
{
    if (request->kind != UK_FILE_LOCK_PROCESS)
        return 0;
    for (struct lock_waiter *waiter = waiting; waiter; waiter = waiter->next)
        waiter->reached = waiter->request->kind == UK_FILE_LOCK_PROCESS &&
                          waiter->request->owner == request->owner;
    int changed;
    do {
        changed = 0;
        for (struct lock_waiter *waiter = waiting; waiter; waiter = waiter->next) {
            if (!waiter->reached)
                continue;
            for (struct file_lock *lock = held; lock; lock = lock->next) {
                if (lock->value.kind != UK_FILE_LOCK_PROCESS ||
                    !conflicts(waiter->request, &lock->value))
                    continue;
                if (lock->value.owner == request->owner)
                    return 1;
                for (struct lock_waiter *next = waiting; next; next = next->next) {
                    if (!next->reached && next->request->kind == UK_FILE_LOCK_PROCESS &&
                        next->request->owner == lock->value.owner) {
                        next->reached = 1;
                        changed = 1;
                    }
                }
            }
        }
    } while (changed);
    return 0;
}

int uk_file_lock_apply(const struct uk_file_lock_request *request,
                       struct flock *query, int block)
{
    struct lock_waiter waiter = { .request = request };
    struct uk_mutex *mutex = &lock_mutex;
    int registered = 0, result = 0;
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
    struct uk_posix_signal_wait signal;
    if (block) {
        result = uk_posix_signal_wait_init(&signal, &lock_waitq, NULL, 0);
        if (result)
            return result;
    }
#endif
    uk_mutex_lock(&lock_mutex);
    if (request->kind == UK_FILE_LOCK_FLOCK && request->type != F_UNLCK) {
        for (struct file_lock *lock = held; lock; lock = lock->next) {
            if (same_owner(request, &lock->value) && request->type == lock->value.type)
                goto out;
        }
        remove_owner(request->file, request->owner, request->kind);
    }
    for (;;) {
        struct file_lock *conflict = request->type == F_UNLCK ? NULL : find_conflict(request);
        if (query) {
            if (!conflict) {
                query->l_type = F_UNLCK;
            } else {
                query->l_type = conflict->value.type;
                query->l_whence = SEEK_SET;
                query->l_start = conflict->value.start;
                query->l_len = conflict->value.end == LLONG_MAX ? 0 :
                               conflict->value.end - conflict->value.start + 1;
                query->l_pid = conflict->value.pid;
            }
            break;
        }
        if (!conflict) {
            result = replace_range(request);
            break;
        }
        if (!block) {
            result = -EAGAIN;
            break;
        }
        if (!registered) {
            waiter.next = waiting;
            waiting = &waiter;
            registered = 1;
        }
        if (deadlocked(request)) {
            result = -EDEADLK;
            break;
        }
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
        uk_mutex_unlock(&lock_mutex);
        result = uk_posix_signal_wait_check(&signal);
        uk_mutex_lock(&lock_mutex);
        if (result)
            break;
        if (!find_conflict(request))
            continue;
        uk_waitq_wait_event_locked(&lock_waitq,
            uk_load_n(&signal.interrupted) || !find_conflict(request),
            uk_mutex_lock, uk_mutex_unlock, mutex);
#else
        uk_waitq_wait_event_locked(&lock_waitq, !find_conflict(request),
                                  uk_mutex_lock, uk_mutex_unlock, mutex);
#endif
    }
out:
    if (registered) {
        struct lock_waiter **entry = &waiting;
        while (*entry != &waiter)
            entry = &(*entry)->next;
        *entry = waiter.next;
    }
    uk_mutex_unlock(&lock_mutex);
#if CONFIG_LIBPOSIX_PROCESS_SIGNAL
    if (block)
	uk_posix_signal_wait_end(&signal, result,
				 UK_POSIX_SIGNAL_RESTART_HANDLER);
#endif
    return result;
}
