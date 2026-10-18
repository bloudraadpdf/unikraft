/* SPDX-License-Identifier: BSD-3-Clause */
#include <limits.h>
#include <string.h>
#include <sys/uio.h>
#include <uk/arch/limits.h>
#include <uk/posix-signal-wait.h>
#include <uk/posix-sem.h>
#include "sysv.h"

#define SEM_UNDO 010000
#define GETPID 11
#define GETVAL 12
#define GETALL 13
#define GETNCNT 14
#define GETZCNT 15
#define SETVAL 16
#define SETALL 17
#define SEM_STAT 18
#define SEM_INFO 19
#define SEM_STAT_ANY 20
#define SEMVMX 32767
#define SEMMSL 32768
#define SEMOPM 500

struct sem_information {
	int semmap, semmni, semmns, semmnu, semmsl;
	int semopm, semume, semusz, semvmx, semaem;
};

UK_CTASSERT(sizeof(struct sem_information) == 40);
UK_CTASSERT(SEMMSL <= INT_MAX / SYSV_IPC_MAX_IDS);

static struct uk_posix_sem_limits sem_limits = {
	.per_set = SEMMSL,
	.total = SEMMSL * SYSV_IPC_MAX_IDS,
	.operations = SEMOPM,
	.sets = SYSV_IPC_MAX_IDS,
};

static unsigned int used_semaphores;

void uk_posix_sem_limits_get(struct uk_posix_sem_limits *limits)
{
	uk_mutex_lock(&pprocess_mutex);
	*limits = sem_limits;
	uk_mutex_unlock(&pprocess_mutex);
}

static bool sem_space(char value)
{
	return value == ' ' || value == '\t' || value == '\n' ||
	       value == '\r' || value == '\v' || value == '\f';
}

static int sem_digit(char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

static int sem_parse_integer(const char *text, size_t length, size_t *cursor,
			     int *value)
{
	size_t position = *cursor, digits;
	unsigned int magnitude = 0, base = 10;
	bool negative = text[position] == '-';
	const unsigned int limit = (unsigned int)INT_MAX + negative;
	int digit;

	if (negative)
		position++;
	if (position < length && text[position] == '0') {
		base = 8;
		if (position + 1 < length &&
		    (text[position + 1] == 'x' || text[position + 1] == 'X')) {
			base = 16;
			position += 2;
		}
	}
	digits = position;
	while (position < length && (digit = sem_digit(text[position])) >= 0 &&
	       (unsigned int)digit < base) {
		if (magnitude > (limit - digit) / base)
			return -EINVAL;
		magnitude = magnitude * base + digit;
		position++;
	}
	if (position == digits || position - *cursor >= 22 ||
	    (position < length && text[position] != ' ' &&
	     text[position] != '\t' && text[position] != '\n'))
		return -EINVAL;
	*value = negative ? (int)-(__s64)magnitude : (int)magnitude;
	*cursor = position;
	return 0;
}

ssize_t uk_posix_sem_limits_write(const struct iovec *iov, size_t count,
				  size_t offset)
{
	char buffer[__PAGE_SIZE - 1];
	size_t total = 0, length = 0, cursor = 0, parsed = 0;
	int values[4], rc = 0;
	int *fields[] = {&sem_limits.per_set, &sem_limits.total,
			 &sem_limits.operations, &sem_limits.sets};

	if (uk_syscall_r_geteuid())
		return -EACCES;
	for (size_t i = 0; i < count; i++) {
		size_t bytes = MIN(iov[i].iov_len, sizeof(buffer) - length);

		if (iov[i].iov_len > (size_t)SSIZE_MAX - total)
			return -EINVAL;
		total += iov[i].iov_len;
		if (bytes && uk_nofault_memcpy(buffer + length, iov[i].iov_base,
					       bytes, 0) != bytes)
			return -EFAULT;
		length += bytes;
	}
	if (!length || offset)
		return total;
	while (parsed < ARRAY_SIZE(values)) {
		while (cursor < length && sem_space(buffer[cursor]))
			cursor++;
		if (cursor == length)
			break;
		rc = sem_parse_integer(buffer, length, &cursor,
				       &values[parsed]);
		if (rc)
			break;
		parsed++;
	}
	if (!parsed)
		return -EINVAL;
	if (parsed == ARRAY_SIZE(values) &&
	    (values[3] < 0 || values[3] > SYSV_IPC_MAX_IDS)) {
		parsed--;
		rc = -ERANGE;
	}
	uk_mutex_lock(&pprocess_mutex);
	for (size_t i = 0; i < parsed; i++)
		*fields[i] = values[i];
	uk_mutex_unlock(&pprocess_mutex);
	if (rc)
		return rc;
	while (cursor < length && sem_space(buffer[cursor]))
		cursor++;
	return total - length + cursor;
}

static int sem_information(int command, unsigned long argument)
{
	struct sem_information info = {0};
	unsigned int sets;
	int highest;

	uk_mutex_lock(&pprocess_mutex);
	highest = sysv_table_info_locked(SYSV_SEM, &sets);
	info.semmni = sem_limits.sets;
	info.semmns = sem_limits.total;
	info.semmsl = sem_limits.per_set;
	info.semopm = sem_limits.operations;
	info.semvmx = SEMVMX;
	info.semmap = SEMMSL * SYSV_IPC_MAX_IDS;
	info.semmnu = info.semmap;
	info.semume = SEMOPM;
	info.semusz = command == SEM_INFO ? sets : 20;
	info.semaem = command == SEM_INFO ? used_semaphores : SEMVMX;
	uk_mutex_unlock(&pprocess_mutex);
	if (!uk_nofault_try_write(info, (void *)argument))
		return -EFAULT;
	return highest;
}

struct sysv_semid {
	struct sysv_perm perm;
	long otime;
#if CONFIG_ARCH_X86_64
	unsigned long unused_otime;
#endif
	long ctime;
#if CONFIG_ARCH_X86_64
	unsigned long unused_ctime;
#endif
	unsigned long count;
	unsigned long unused[2];
};

struct sysv_sembuf {
	unsigned short num;
	short op, flags;
};

UK_CTASSERT(sizeof(struct sysv_sembuf) == 6);
#if CONFIG_ARCH_X86_64
UK_CTASSERT(sizeof(struct sysv_semid) == 104);
#else
UK_CTASSERT(sizeof(struct sysv_semid) == 88);
#endif

struct sem_value {
	int value;
	pid_t pid;
	unsigned int ncnt, zcnt;
};

struct sem_set {
	struct sysv_object object;
	struct sysv_semid status;
	struct uk_waitq changed;
	struct sem_value values[];
};

struct sem_adjustments {
	struct uk_list_head link;
	struct sem_set *set;
	int values[];
};

struct sysv_sem_undo {
	struct uk_list_head link;
	struct uk_list_head entries;
	unsigned int references;
};

struct sem_work {
	unsigned int num;
	int value, adjustment;
};

static UK_LIST_HEAD(undo_states);

static struct sysv_sem_undo *undo_new(void)
{
	struct sysv_sem_undo *state = uk_zalloc(uk_alloc_get_default(),
					      sizeof(*state));

	if (state) {
		state->references = 1;
		UK_INIT_LIST_HEAD(&state->entries);
		uk_list_add_tail(&state->link, &undo_states);
	}
	return state;
}

static struct sem_adjustments *undo_get(struct sem_set *set)
{
	struct posix_thread *thread = uk_pthread_current();
	struct sem_adjustments *entry;

	if (!thread->sem_undo) {
		thread->sem_undo = undo_new();
		if (!thread->sem_undo)
			return NULL;
	}
	uk_list_for_each_entry(entry, &thread->sem_undo->entries, link)
		if (entry->set == set)
			return entry;
	entry = uk_zalloc(uk_alloc_get_default(), sizeof(*entry) +
			 set->status.count * sizeof(entry->values[0]));
	if (entry) {
		entry->set = set;
		set->object.references++;
		uk_list_add_tail(&entry->link, &thread->sem_undo->entries);
	}
	return entry;
}

static void undo_clear(struct sem_set *set, int num)
{
	struct sysv_sem_undo *state;
	struct sem_adjustments *entry;

	uk_list_for_each_entry(state, &undo_states, link)
		uk_list_for_each_entry(entry, &state->entries, link) {
			if (entry->set != set)
				continue;
			if (num < 0)
				memset(entry->values, 0, set->status.count *
				       sizeof(entry->values[0]));
			else
				entry->values[num] = 0;
		}
}

static void sem_destroy(struct sysv_object *object)
{
	uk_free(uk_alloc_get_default(), object);
}

static void undo_remove(struct sem_set *set)
{
	struct sysv_sem_undo *state;
	struct sem_adjustments *entry, *next;

	uk_list_for_each_entry(state, &undo_states, link) {
		uk_list_for_each_entry_safe(entry, next,
					    &state->entries, link) {
			if (entry->set != set)
				continue;
			uk_list_del(&entry->link);
			sysv_put(&set->object);
			uk_free(uk_alloc_get_default(), entry);
		}
	}
}

UK_LLSYSCALL_R_DEFINE(int, semget, int, key, int, count, int, flags)
{
	struct sysv_object *object;
	struct sem_set *set;
	unsigned int sets;
	int rc;

	uk_mutex_lock(&pprocess_mutex);
	if (count < 0 || count > sem_limits.per_set) {
		rc = -EINVAL;
		goto out;
	}
	object = sysv_find_key(SYSV_SEM, key, flags);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	if (object) {
		set = (struct sem_set *)object;
		rc = (unsigned int)count > set->status.count ?
			-EINVAL : object->id;
		goto out;
	}
	if (!count) {
		rc = -EINVAL;
		goto out;
	}
	sysv_table_info_locked(SYSV_SEM, &sets);
	if (sets >= (unsigned int)sem_limits.sets || sem_limits.total < 0 ||
	    used_semaphores > (unsigned int)sem_limits.total ||
	    (unsigned int)count >
		    (unsigned int)sem_limits.total - used_semaphores) {
		rc = -ENOSPC;
		goto out;
	}
	if ((unsigned int)count >
	    (INT_MAX - sizeof(*set)) / sizeof(set->values[0])) {
		rc = -ENOMEM;
		goto out;
	}
	set = uk_zalloc(uk_alloc_get_default(), sizeof(*set) +
			count * sizeof(set->values[0]));
	if (!set) {
		rc = -ENOMEM;
		goto out;
	}
	set->object.destroy = sem_destroy;
	set->status.count = count;
	set->status.ctime = sysv_now();
	uk_waitq_init(&set->changed);
	rc = sysv_insert(&set->object, SYSV_SEM, key, flags);
	if (rc < 0)
		sem_destroy(&set->object);
	else
		used_semaphores += count;
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static int sem_apply(struct sem_set *set, const struct sysv_sembuf *ops,
		     unsigned int count, struct sem_work *work,
		     struct sem_adjustments *undo, unsigned int *blocked)
{
	unsigned int i, j, used = 0;
	pid_t pid = uk_pprocess_current()->pid;

	for (i = 0; i < count; i++) {
		if (ops[i].num >= set->status.count)
			return -EFBIG;
		for (j = 0; j < used; j++)
			if (work[j].num == ops[i].num)
				break;
		if (j == used) {
			work[j] = (struct sem_work) {
				.num = ops[i].num,
				.value = set->values[ops[i].num].value,
				.adjustment = 0,
			};
			if (undo)
				work[j].adjustment = undo->values[ops[i].num];
			used++;
		}
		if ((!ops[i].op && work[j].value) ||
		    (ops[i].op < 0 && work[j].value < -ops[i].op)) {
			*blocked = i;
			return ops[i].flags & SYSV_IPC_NOWAIT ? -EAGAIN : 1;
		}
		work[j].value += ops[i].op;
		if (work[j].value > SEMVMX)
			return -ERANGE;
		if (ops[i].flags & SEM_UNDO) {
			work[j].adjustment -= ops[i].op;
			if (work[j].adjustment < -SEMVMX ||
			    work[j].adjustment > SEMVMX)
				return -ERANGE;
		}
	}
	for (i = 0; i < used; i++) {
		set->values[work[i].num].value = work[i].value;
		set->values[work[i].num].pid = pid;
		if (undo)
			undo->values[work[i].num] = work[i].adjustment;
	}
	set->status.otime = sysv_now();
	uk_waitq_wake_up(&set->changed);
	return 0;
}

static int sem_deadline(const struct timespec *interval, __nsec *deadline)
{
	__nsec now, duration, subsecond, maximum = (__nsec)LONG_MAX;

	if (!interval) {
		*deadline = 0;
		return 0;
	}
	if (interval->tv_sec < 0 || interval->tv_nsec < 0 ||
	    (unsigned long)interval->tv_nsec >= UKARCH_NSEC_PER_SEC)
		return -EINVAL;
	subsecond = interval->tv_nsec;
	now = ukplat_monotonic_clock();
	if ((__nsec)interval->tv_sec >
	    (maximum - subsecond) / UKARCH_NSEC_PER_SEC)
		duration = maximum;
	else
		duration = (__nsec)interval->tv_sec * UKARCH_NSEC_PER_SEC +
			   subsecond;
	*deadline = duration > maximum - now ? maximum : now + duration;
	return 0;
}

static int sem_operate(int id, const struct sysv_sembuf *operations,
		       size_t count, const struct timespec *timeout)
{
	struct timespec interval;
	struct uk_posix_signal_wait sw;
	struct uk_mutex *mutex = &pprocess_mutex;
	struct sysv_object *object;
	struct sem_set *set;
	struct sem_adjustments *undo = NULL;
	struct sysv_sembuf *ops;
	struct sem_work *work;
	__nsec deadline;
	unsigned int i, blocked = 0, mode = 4, *waiting;
	bool needs_undo = false;
	int rc;

	if (timeout && !uk_nofault_try_read(interval, timeout))
		return -EFAULT;
	if (!count)
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	rc = count > (unsigned int)sem_limits.operations ? -E2BIG : 0;
	uk_mutex_unlock(&pprocess_mutex);
	if (rc)
		return rc;
	ops = uk_malloc(uk_alloc_get_default(), count * sizeof(*ops));
	work = uk_malloc(uk_alloc_get_default(), count * sizeof(*work));
	if (!ops || !work) {
		rc = -ENOMEM;
		goto free;
	}
	size_t size = count * sizeof(*ops);

	size_t copied = uk_nofault_memcpy((char *)ops,
					(const char *)operations, size, 0);

	if (copied != size) {
		rc = -EFAULT;
		goto free;
	}
	rc = sem_deadline(timeout ? &interval : NULL, &deadline);
	if (rc)
		goto free;
	for (i = 0; i < count; i++) {
		if (ops[i].op)
			mode = 2;
		needs_undo |= !!(ops[i].flags & SEM_UNDO);
	}
	uk_mutex_lock(mutex);
	object = sysv_find_id(SYSV_SEM, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto unlock;
	}
	set = (struct sem_set *)object;
	rc = sysv_access(object, mode);
	if (rc)
		goto unlock;
	for (i = 0; i < count; i++) {
		if (ops[i].num >= set->status.count) {
			rc = -EFBIG;
			goto unlock;
		}
	}
	if (needs_undo) {
		undo = undo_get(set);
		if (!undo) {
			rc = -ENOMEM;
			goto unlock;
		}
	}
	object->references++;
	uk_posix_signal_wait_init(&sw, &set->changed, NULL, 0);
	for (;;) {
		if (object->removed) {
			rc = -EIDRM;
			break;
		}
		rc = sem_apply(set, ops, count, work, undo, &blocked);
		if (rc != 1)
			break;
		rc = uk_posix_signal_wait_check(&sw);
		if (rc)
			break;
		if (timeout && ukplat_monotonic_clock() >= deadline) {
			rc = -EAGAIN;
			break;
		}
		struct sem_value *value = &set->values[ops[blocked].num];

		waiting = ops[blocked].op ? &value->ncnt : &value->zcnt;
		(*waiting)++;
		uk_waitq_wait_deadline_unless_locked(&set->changed,
						     uk_load_n(&sw.interrupted),
						     deadline, uk_mutex_lock,
						     uk_mutex_unlock,
						     mutex);
		(*waiting)--;
		if (uk_posix_signal_wait_check(&sw)) {
			rc = -EINTR;
			break;
		}
	}
	uk_posix_signal_wait_end(&sw, rc, UK_POSIX_SIGNAL_RESTART_NONE);
	sysv_put(object);
unlock:
	uk_mutex_unlock(mutex);
free:
	uk_free(uk_alloc_get_default(), work);
	uk_free(uk_alloc_get_default(), ops);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, semop, int, id,
		      const struct sysv_sembuf *, operations,
		      unsigned int, count)
{
	return sem_operate(id, operations, count, NULL);
}

UK_LLSYSCALL_R_DEFINE(int, semtimedop, int, id,
		      const struct sysv_sembuf *, operations,
		      unsigned int, count,
		      const struct timespec *, timeout)
{
	return sem_operate(id, operations, count, timeout);
}

static int sem_array(struct sem_set *set, int command, unsigned long argument)
{
	size_t size = set->status.count * sizeof(unsigned short);
	unsigned short *values = uk_malloc(uk_alloc_get_default(), size);
	char *bytes = (char *)values;
	char *user = (char *)argument;
	unsigned int i;
	int rc = 0;

	if (!values)
		return -ENOMEM;
	if (command == SETALL) {
		if (uk_nofault_memcpy(bytes, user, size, 0) != size) {
			rc = -EFAULT;
			goto out;
		}
		for (i = 0; i < set->status.count; i++)
			if (values[i] > SEMVMX) {
				rc = -ERANGE;
				goto out;
			}
		for (i = 0; i < set->status.count; i++) {
			set->values[i].value = values[i];
			set->values[i].pid = uk_pprocess_current()->pid;
		}
		undo_clear(set, -1);
		set->status.ctime = sysv_now();
		uk_waitq_wake_up(&set->changed);
	} else {
		for (i = 0; i < set->status.count; i++)
			values[i] = set->values[i].value;
		if (uk_nofault_memcpy(user, bytes, size, 0) != size)
			rc = -EFAULT;
	}
out:
	uk_free(uk_alloc_get_default(), values);
	return rc;
}

UK_LLSYSCALL_R_DEFINE(int, semctl, int, id, int, num, int, command,
		      unsigned long, argument)
{
	struct sysv_object *object;
	struct sem_set *set;
	struct sysv_semid status;
	int rc;

	if (id < 0)
		return -EINVAL;
	if (command == SYSV_IPC_INFO || command == SEM_INFO)
		return sem_information(command, argument);
	if ((command < SYSV_IPC_RMID || command > SYSV_IPC_STAT) &&
	    (command < GETPID || command > SEM_STAT_ANY))
		return -EINVAL;
	uk_mutex_lock(&pprocess_mutex);
	object = command == SEM_STAT || command == SEM_STAT_ANY ?
		sysv_find_index(SYSV_SEM, id) : sysv_find_id(SYSV_SEM, id);
	if (PTRISERR(object)) {
		rc = PTR2ERR(object);
		goto out;
	}
	set = (struct sem_set *)object;
	switch (command) {
	case SYSV_IPC_RMID:
		rc = sysv_owner(object);
		if (!rc) {
			UK_ASSERT(used_semaphores >= set->status.count);
			used_semaphores -= set->status.count;
			undo_remove(set);
			uk_waitq_wake_up(&set->changed);
			sysv_remove(object);
		}
		goto out;
	case SYSV_IPC_SET:
		if (!uk_nofault_try_read(status, (void *)argument)) {
			rc = -EFAULT;
			goto out;
		}
		rc = sysv_set_perm(object, &status.perm);
		if (!rc)
			set->status.ctime = sysv_now();
		goto out;
	case SYSV_IPC_STAT:
	case SEM_STAT:
	case SEM_STAT_ANY:
		rc = command == SEM_STAT_ANY ? 0 : sysv_access(object, 4);
		if (!rc) {
			status = set->status;
			status.perm = object->perm;
			rc = command == SYSV_IPC_STAT ? 0 : object->id;
			if (!uk_nofault_try_write(status, (void *)argument))
				rc = -EFAULT;
		}
		goto out;
	}
	rc = sysv_access(object,
			 command == SETVAL || command == SETALL ? 2 : 4);
	if (rc)
		goto out;
	if (command == GETALL || command == SETALL) {
		rc = sem_array(set, command, argument);
		goto out;
	}
	if (num < 0 || (unsigned int)num >= set->status.count) {
		rc = -EINVAL;
		goto out;
	}
	switch (command) {
	case GETPID:
		rc = set->values[num].pid;
		break;
	case GETVAL:
		rc = set->values[num].value;
		break;
	case GETNCNT:
		rc = set->values[num].ncnt;
		break;
	case GETZCNT:
		rc = set->values[num].zcnt;
		break;
	case SETVAL:
		if ((int)argument < 0 || (int)argument > SEMVMX) {
			rc = -ERANGE;
			break;
		}
		set->values[num].value = (int)argument;
		set->values[num].pid = uk_pprocess_current()->pid;
		set->status.ctime = sysv_now();
		undo_clear(set, num);
		uk_waitq_wake_up(&set->changed);
		rc = 0;
		break;
	default:
		rc = -EINVAL;
	}
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static int sem_undo_clone(void *arg)
{
	struct posix_process_clone_event_data *event = arg;
	struct posix_thread *parent, *child;
	int rc = UK_EVENT_HANDLED_CONT;

	if (!event->parent || !(event->cl_args->flags & CLONE_SYSVSEM))
		return UK_EVENT_NOT_HANDLED;
	uk_mutex_lock(&pprocess_mutex);
	parent = uk_thread_uktls_var(event->parent, pthread_self);
	child = tid2pthread(event->tid);
	if (!parent || !child) {
		rc = -ESRCH;
		goto out;
	}
	if (!parent->sem_undo)
		parent->sem_undo = undo_new();
	if (!parent->sem_undo) {
		rc = -ENOMEM;
	} else {
		parent->sem_undo->references++;
		child->sem_undo = parent->sem_undo;
	}
out:
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

static int sem_undo_exit(void *arg)
{
	struct posix_process_ptexit_event_data *event = arg;
	struct posix_thread *thread;
	struct sysv_sem_undo *state;
	struct sem_adjustments *entry, *next;
	unsigned int i;

	uk_mutex_lock(&pprocess_mutex);
	thread = tid2pthread(event->tid);
	if (!thread)
		goto out;
	state = thread->sem_undo;
	thread->sem_undo = NULL;
	if (!state || --state->references)
		goto out;
	uk_list_for_each_entry_safe(entry, next, &state->entries, link) {
		struct sem_set *set = entry->set;

		if (!set->object.removed) {
			for (i = 0; i < set->status.count; i++) {
				if (!entry->values[i])
					continue;
				struct sem_value *value = &set->values[i];
				int adjusted = value->value + entry->values[i];

				value->value = MAX(0, MIN(SEMVMX, adjusted));
				value->pid = thread->process->pid;
			}
			set->status.otime = sysv_now();
			uk_waitq_wake_up(&set->changed);
		}
		sysv_put(&set->object);
		uk_list_del(&entry->link);
		uk_free(uk_alloc_get_default(), entry);
	}
	uk_list_del(&state->link);
	uk_free(uk_alloc_get_default(), state);
out:
	uk_mutex_unlock(&pprocess_mutex);
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_CLONE_HANDLER(0, sem_undo_clone);
POSIX_PROCESS_PTEXIT_HANDLER(sem_undo_exit);
