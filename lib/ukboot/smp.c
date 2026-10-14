/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/arch/tls.h>
#include <uk/atomic.h>
#include <uk/lcpu.h>
#include <uk/plat/config.h>
#include <uk/sched.h>
#include <uk/schedcoop.h>
#include <uk/plat/common/_time.h>
#if CONFIG_LIBUKVMEM
#include <uk/vmem.h>
#elif CONFIG_LIBUKPAGING
#include <uk/paging.h>
#endif

struct cpu_boot_context {
	struct uk_sched *scheduler;
#if CONFIG_LIBUKVMEM
	struct uk_vas *vas;
#elif CONFIG_LIBUKPAGING
	struct uk_pagetable *pt;
#endif
	__uptr tlsp;
	__uptr auxsp;
	int result;
};

static struct cpu_boot_context cpu_contexts[CONFIG_UKPLAT_CPU_MAXCOUNT];

static __noreturn void secondary_boot(struct uk_lcpu *cpu)
{
	struct cpu_boot_context *context;
	unsigned int index = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	int rc;

	context = &cpu_contexts[index];
#if CONFIG_LIBUKVMEM
	rc = uk_vas_set_active(context->vas);
#elif CONFIG_LIBUKPAGING
	rc = uk_paging_pt_set_active(context->pt);
#else
	rc = 0;
#endif
	if (rc)
		goto error;
	rc = uk_lcpu_init(cpu);
	if (rc)
		goto error;
	uk_lcpu_tlsp_set(context->tlsp);
	uk_lcpu_set_auxsp(context->auxsp);
#if CONFIG_ARCH_ARM_64
	ukplat_time_cpu_init();
#endif
	rc = uk_sched_start(context->scheduler);
	if (rc)
		goto error;
	uk_thread_block(uk_thread_current());
	uk_dec(&cpu->state);
	uk_sched_yield();
	UK_CRASH("Secondary CPU bootstrap thread resumed\n");
error:
	__atomic_store_n(&context->result, rc, __ATOMIC_RELEASE);
	uk_lcpu_halt();
}

int uk_boot_smp_init(struct uk_sched *primary)
{
	for (__u64 index = 1; index < uk_lcpu_count(); ++index) {
		struct cpu_boot_context *context = &cpu_contexts[index];
		unsigned int count = 1;
		unsigned long irq_flags;
		__uptr entry = (__uptr)secondary_boot;
		__uptr stack;
		void *base, *auxstack, *tls;
		int rc;

#if CONFIG_LIBUKVMEM
		context->vas = uk_vas_get_active();
#elif CONFIG_LIBUKPAGING
		context->pt = uk_paging_pt_get_active();
#endif
		context->scheduler = uk_schedcoop_create(primary->a, primary->a_stack,
			primary->a_auxstack, primary->a_uktls);
		if (!context->scheduler)
			return -ENOMEM;
		tls = uk_memalign(primary->a_uktls, ukarch_tls_area_align(), ukarch_tls_area_size());
		base = uk_memalign(primary->a_stack, UKARCH_SP_ALIGN, STACK_SIZE);
		auxstack = uk_memalign(primary->a_auxstack, UKARCH_AUXSP_ALIGN, AUXSTACK_SIZE);
		if (!tls || !base || !auxstack)
			return -ENOMEM;
		ukarch_tls_area_init(tls);
		context->tlsp = ukarch_tls_tlsp(tls);
		context->auxsp = ukarch_gen_sp(auxstack, AUXSTACK_SIZE);
		ukarch_auxsp_init(context->auxsp);
		ukarch_auxspcb_set_uktlsp(ukarch_auxsp_get_cb(context->auxsp), context->tlsp);
		stack = ukarch_gen_sp(base, STACK_SIZE);
		irq_flags = uk_lcpu_save_irqf();
		rc = uk_lcpu_start(&index, &count, &stack, &entry, 0);
		if (!rc && count == 1) {
			count = 1;
			rc = uk_lcpu_wait(&index, &count, 0);
		}
		uk_lcpu_restore_irqf(irq_flags);
		if (rc || count != 1)
			return rc ? rc : -EIO;
		rc = __atomic_load_n(&context->result, __ATOMIC_ACQUIRE);
		if (rc)
			return rc;
	}
	return 0;
}
