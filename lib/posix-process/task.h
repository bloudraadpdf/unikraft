/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __PPROCESS_TASK_H__
#define __PPROCESS_TASK_H__

#include <uk/thread.h>
#include <uk/process.h>

struct posix_task {
	char name[UK_POSIX_TASK_NAME_SIZE];
};

int pprocess_task_init(struct posix_task *task, struct uk_thread *thread,
		       struct uk_thread *creator);
int pprocess_task_name_get(struct uk_thread *thread,
			   char name[UK_POSIX_TASK_NAME_SIZE]);
int pprocess_task_name_set(struct uk_thread *thread,
			   const char name[UK_POSIX_TASK_NAME_SIZE]);
void pprocess_task_name_from_path(const char *path,
				  char name[UK_POSIX_TASK_NAME_SIZE]);

#endif /* __PPROCESS_TASK_H__ */
