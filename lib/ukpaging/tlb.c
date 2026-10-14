/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <uk/paging.h>
#include <uk/pal/except.h>
#if CONFIG_ARCH_X86_64 && CONFIG_HAVE_SMP
#include <uk/lcpu.h>
#include <uk/atomic.h>
#include <uk/pcpuvar.h>

struct tlb_request {
	__vaddr_t address;
	int pending;
};

static __uk_pcpuvar __used struct tlb_request tlb_request;
static int tlb_lock;

void uk_paging_tlb_service(void)
{
	struct tlb_request *request = uk_pcpuvar_current_ptr_get(tlb_request);

	UK_ASSERT(uk_lcpu_irqs_disabled());
	if (!__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE))
		return;
	if (request->address == UK_PAGING_VADDR_INV)
		uk_pal_tlb_flush();
	else
		uk_pal_tlb_flush_entry(request->address);
	__atomic_store_n(&request->pending, 0, __ATOMIC_RELEASE);
}

static void tlb_remote(struct uk_lcpu_regs *regs __unused, void *arg __unused)
{
	uk_paging_tlb_service();
}

static void tlb_flush(__vaddr_t address)
{
	unsigned long flags = uk_pal_save_irqf();
	const struct uk_lcpu_func fn = { .fn = tlb_remote };
	__u64 self = uk_pcpuvar_current_get(uk_pcpuvar_cpu_idx);
	unsigned int count = uk_lcpu_count();

	while (__atomic_exchange_n(&tlb_lock, 1, __ATOMIC_ACQUIRE)) {
		uk_paging_tlb_service();
		uk_arch_spinwait();
	}
	if (address == UK_PAGING_VADDR_INV)
		uk_pal_tlb_flush();
	else
		uk_pal_tlb_flush_entry(address);
	for (__u64 cpu = 0; cpu < count; cpu++) {
		struct tlb_request *request =
			&uk_pcpuvar_lval(cpu, tlb_request);
		struct uk_lcpu *lcpu = &uk_pcpuvar_lval(cpu, uk_lcpus);
		int sent = 0;

		if (cpu == self ||
		    !uk_lcpu_state_is_online(uk_load_n(&lcpu->state)))
			continue;
		request->address = address;
		__atomic_store_n(&request->pending, 1, __ATOMIC_RELEASE);
		while (__atomic_load_n(&request->pending, __ATOMIC_ACQUIRE)) {
			unsigned int nr = 1;
			int rc;

			if (!uk_lcpu_state_is_online(uk_load_n(&lcpu->state))) {
				__atomic_store_n(&request->pending, 0,
						 __ATOMIC_RELEASE);
				break;
			}
			if (!sent) {
				rc = uk_lcpu_run(&cpu, &nr, &fn,
						 UK_LCPU_RFLG_DONOTBLOCK);
				if (!rc)
					sent = 1;
				else if (rc != -EAGAIN)
					UK_CRASH("TLB IPI failed (%d)\n", rc);
			}
			uk_arch_spinwait();
		}
	}
	__atomic_store_n(&tlb_lock, 0, __ATOMIC_RELEASE);
	uk_pal_restore_irqf(flags);
}
#else
static void tlb_flush(__vaddr_t address)
{
	if (address == UK_PAGING_VADDR_INV)
		uk_pal_tlb_flush();
	else
		uk_pal_tlb_flush_entry(address);
}
#endif

void uk_paging_tlb_flush_entry(__vaddr_t address)
{
	tlb_flush(address);
}

void uk_paging_tlb_flush(void)
{
	tlb_flush(UK_PAGING_VADDR_INV);
}
