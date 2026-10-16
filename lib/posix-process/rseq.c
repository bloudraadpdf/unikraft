/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <signal.h>
#include <uk/arch/ctx.h>
#include <uk/lcpu.h>
#include <uk/nofault.h>
#if CONFIG_HAVE_PAGING
#include <uk/paging.h>
#endif
#include <uk/process.h>
#include <uk/sched.h>
#include <uk/syscall.h>

#include "process.h"
#include "application.h"
#include "rseq.h"

#define RSEQ_SIZE 32
#define RSEQ_UNREGISTER 1

struct rseq_area {
	__u32 cpu_id_start;
	__u32 cpu_id;
	__u64 rseq_cs;
	__u32 flags;
};

struct rseq_cs {
	__u32 version;
	__u32 flags;
	__u64 start_ip;
	__u64 post_commit_offset;
	__u64 abort_ip;
};

static int update_cpu(struct rseq_area *area, bool unregister)
{
	__u32 cpu = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	__u32 start = unregister ? 0 : cpu;
	__u32 current = unregister ? (__u32)-1 : cpu;

	return uk_nofault_try_write(start, &area->cpu_id_start) &&
		uk_nofault_try_write(current, &area->cpu_id) ? 0 : -EFAULT;
}

UK_LLSYSCALL_R_DEFINE(int, rseq, struct rseq_area *, area, __u32, length,
		      int, flags, __u32, signature)
{
	struct posix_thread *thread = uk_pthread_current();
	__u64 zero = 0;
	__u32 clear_flags = 0;
	int rc;

	if (flags & ~RSEQ_UNREGISTER)
		return -EINVAL;
	if (thread->rseq_area) {
		if (area != thread->rseq_area || length != thread->rseq_length)
			return -EINVAL;
		if (signature != thread->rseq_signature)
			return -EPERM;
		if (!flags)
			return -EBUSY;
		rc = update_cpu(area, true);
		if (rc)
			return rc;
		thread->rseq_area = NULL;
		thread->rseq_length = 0;
		thread->rseq_signature = 0;
		return 0;
	}
	if (flags || length < RSEQ_SIZE || (__uptr)area % RSEQ_SIZE)
		return -EINVAL;
	if ((__uptr)area > __PTR_MAX - length)
		return -EFAULT;
#if CONFIG_HAVE_PAGING
	if (!uk_paging_vaddr_range_isvalid((__vaddr_t)area, length))
		return -EFAULT;
#endif
	if (uk_nofault_probe_rw((__vaddr_t)area, RSEQ_SIZE, 0) != RSEQ_SIZE)
		return -EFAULT;
	if (!uk_nofault_try_write(zero, &area->rseq_cs) ||
	    !uk_nofault_try_write(clear_flags, &area->flags))
		return -EFAULT;
	rc = update_cpu(area, false);
	if (rc)
		return rc;
	thread->rseq_area = area;
	thread->rseq_length = length;
	thread->rseq_signature = signature;
	return 0;
}

static int interrupt_sequence(struct posix_thread *thread,
			      struct ukarch_execenv *execenv)
{
	struct rseq_area *area = thread->rseq_area;
	struct rseq_cs cs;
	__u64 address, zero = 0;
	__u32 signature, flags;
	__uptr pc = uk_lcpu_regs_get(execenv->regs, PC);

	if (!uk_nofault_try_read(address, &area->rseq_cs))
		return -EFAULT;
	if (!address)
		return update_cpu(area, false);
	if (!uk_nofault_try_read(cs, (void *)(__uptr)address))
		return -EFAULT;
	if (pc - cs.start_ip < cs.post_commit_offset) {
		const __u32 *signature_address;

		if (cs.version || cs.post_commit_offset > __PTR_MAX - cs.start_ip ||
		    cs.abort_ip < sizeof(signature) ||
		    cs.abort_ip - cs.start_ip < cs.post_commit_offset)
			return -EINVAL;
		signature_address = (const void *)(__uptr)
			(cs.abort_ip - sizeof(signature));
		if (!uk_nofault_try_read(signature, signature_address) ||
		    signature != thread->rseq_signature)
			return -EINVAL;
		if (!uk_nofault_try_read(flags, &area->flags))
			return -EFAULT;
		if (flags || cs.flags)
			return -EINVAL;
		uk_lcpu_regs_set(execenv->regs, PC, cs.abort_ip);
	}
	if (!uk_nofault_try_write(zero, &area->rseq_cs))
		return -EFAULT;
	return update_cpu(area, false);
}

void pprocess_rseq_interrupt(struct ukarch_execenv *execenv)
{
	struct posix_thread *thread = uk_pthread_current();

	if (!thread || !thread->rseq_area || !execenv ||
	    !pprocess_application_context(execenv->regs))
		return;
	if (interrupt_sequence(thread, execenv)) {
		pprocess_exit(thread->process, POSIX_PROCESS_KILLED, SIGSEGV);
		uk_sched_thread_exit();
	}
}

#if CONFIG_LIBPOSIX_PROCESS_EXECVE
static int rseq_exec(void *arg)
{
	struct posix_process_execve_event_data *event = arg;
	struct posix_thread *thread = tid2pthread(event->tid);

	if (!thread)
		return UK_EVENT_NOT_HANDLED;
	thread->rseq_area = NULL;
	thread->rseq_length = 0;
	thread->rseq_signature = 0;
	return UK_EVENT_HANDLED_CONT;
}

POSIX_PROCESS_EXECVE_HANDLER_PRIO(rseq_exec, UK_PRIO_EARLIEST);
#endif
