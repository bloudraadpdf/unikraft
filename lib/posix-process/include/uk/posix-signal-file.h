/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef __UK_POSIX_SIGNAL_FILE_H__
#define __UK_POSIX_SIGNAL_FILE_H__

#include <stdbool.h>
#include <uk/file.h>
#include <uk/list.h>

#if CONFIG_LIBUKFILE_CHAINUPDATE
struct uk_posix_signal_file_poll {
	struct uk_list_head process_link;
	struct uk_list_head file_link;
	const struct uk_file *file;
	struct uk_poll_chain *tick;
};

bool uk_posix_signal_file_poll_register(const struct uk_file *file,
		struct uk_posix_signal_file_poll *poll,
		struct uk_poll_chain *tick, uk_pollevent *events);
void uk_posix_signal_file_poll_unregister(struct uk_posix_signal_file_poll *poll);
#endif

#endif
