/* SPDX-License-Identifier: BSD-3-Clause */
#include <string.h>
#include <uk/errptr.h>
#include <uk/init.h>
#include <uk/prio.h>
#include <uk/process.h>
#include "process.h"

struct uk_mutex pprocess_mutex =
	UK_MUTEX_INITIALIZER_RECURSIVE(pprocess_mutex);

#if !CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
static struct uk_thread *application_thread;
static struct posix_task application_task;

static int task_application_init(struct uk_init_ctx *context)
{
	struct uk_thread *thread = context->tmain ?
		context->tmain : uk_thread_current();
	int rc = pprocess_task_init(&application_task, thread, NULL);

	if (!rc)
		application_thread = thread;
	return rc;
}

uk_late_initcall(task_application_init, 0x0);

static void task_application_fini(struct uk_thread *thread)
{
	uk_mutex_lock(&pprocess_mutex);
	if (thread == application_thread)
		application_thread = NULL;
	uk_mutex_unlock(&pprocess_mutex);
}

UK_THREAD_INIT_PRIO(0, task_application_fini, UK_PRIO_EARLIEST);
#endif

static struct posix_task *task_for_thread(struct uk_thread *thread)
{
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_thread *task = uk_thread_uktls_var(thread, pthread_self);

	return task ? &task->task : NULL;
#else
	return thread == application_thread ? &application_task : NULL;
#endif
}

void pprocess_task_name_from_path(const char *path,
				  char name[UK_POSIX_TASK_NAME_SIZE])
{
	const char *base = strrchr(path, '/');

	base = base ? base + 1 : path;
	memset(name, 0, UK_POSIX_TASK_NAME_SIZE);
	memcpy(name, base, strnlen(base, UK_POSIX_TASK_NAME_SIZE - 1));
}

int pprocess_task_init(struct posix_task *task, struct uk_thread *thread,
		       struct uk_thread *creator)
{
	if (creator)
		return pprocess_task_name_get(creator, task->name);
	pprocess_task_name_from_path(thread->name ? thread->name : "",
				     task->name);
	return 0;
}

int pprocess_task_name_get(struct uk_thread *thread,
			   char name[UK_POSIX_TASK_NAME_SIZE])
{
	struct posix_task *task;
	int rc = -ESRCH;

	uk_mutex_lock(&pprocess_mutex);
	task = task_for_thread(thread);
	if (task) {
		memcpy(name, task->name, UK_POSIX_TASK_NAME_SIZE);
		rc = 0;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

int pprocess_task_name_set(struct uk_thread *thread,
			   const char name[UK_POSIX_TASK_NAME_SIZE])
{
	struct posix_task *task;
	int rc = -ESRCH;

	uk_mutex_lock(&pprocess_mutex);
	task = task_for_thread(thread);
	if (task) {
		memcpy(task->name, name, UK_POSIX_TASK_NAME_SIZE);
		rc = 0;
	}
	uk_mutex_unlock(&pprocess_mutex);
	return rc;
}

int uk_posix_process_attach_application(struct uk_thread *thread,
					const char *path)
{
	char name[UK_POSIX_TASK_NAME_SIZE];
#if CONFIG_LIBPOSIX_PROCESS_MULTITHREADING
	struct posix_process *process = uk_pprocess_current();
	struct posix_thread *created;

	if (!process)
		return -ESRCH;
	created = pprocess_create_pthread(process, thread, uk_thread_current());
	if (PTRISERR(created))
		return PTR2ERR(created);
#else
	uk_mutex_lock(&pprocess_mutex);
	if (uk_thread_current() != application_thread) {
		uk_mutex_unlock(&pprocess_mutex);
		return -ESRCH;
	}
	application_thread = thread;
	uk_mutex_unlock(&pprocess_mutex);
#endif
	pprocess_task_name_from_path(path, name);
	return pprocess_task_name_set(thread, name);
}
