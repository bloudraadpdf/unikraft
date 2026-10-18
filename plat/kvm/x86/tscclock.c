/* SPDX-License-Identifier: ISC */
/*
 * Authors: Dan Williams
 *          Martin Lucina
 *          Ricardo Koller
 *          Costin Lupu <costin.lupu@cs.pub.ro>
 *
 * Copyright (c) 2015-2017 IBM
 * Copyright (c) 2016-2017 Docker, Inc.
 * Copyright (c) 2018, NEC Europe Ltd., NEC Corporation
 *
 * Permission to use, copy, modify, and/or distribute this software
 * for any purpose with or without fee is hereby granted, provided
 * that the above copyright notice and this permission notice appear
 * in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* Taken from solo5 tscclock.c */

/*-
 * Copyright (c) 2014, 2015 Antti Kantee.  All Rights Reserved.
 * Copyright (c) 2015 Martin Lucina.  All Rights Reserved.
 * Modified for solo5 by Ricardo Koller <kollerr@us.ibm.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <errno.h>
#include <stdbool.h>

#include <uk/arch/util.h>
#include <uk/lcpu.h>
#include <uk/plat/time.h>
#include <uk/timeconv.h>
#include <uk/print.h>
#include <uk/assert.h>
#include <uk/atomic.h>
#include <uk/paging.h>

#if CONFIG_KVM_PIT
#define TIMER_CNTR           0x40
#define TIMER_MODE           0x43
#define TIMER_SEL0           0x00
#define TIMER_LATCH          0x00
#define TIMER_RATEGEN        0x04
#define TIMER_ONESHOT        0x08
#define TIMER_16BIT          0x30
#define TIMER_HZ             1193182
#endif

#define	RTC_COMMAND          0x70
#define	RTC_DATA             0x71
#define RTC_NMI_DISABLE      (1<<8)
#define RTC_NMI_ENABLE       0
#define	RTC_SEC              0x00
#define	RTC_MIN              0x02
#define	RTC_HOUR             0x04
#define	RTC_DAY              0x07
#define	RTC_MONTH            0x08
#define	RTC_YEAR             0x09
#define	RTC_STATUS_A         0x0a
#define	RTC_UIP              (1<<7)

/*
 * Compile-time check to make sure we don't tick faster than the PIT can go.
 * This is really only a basic sanity check. We'll run into serious issues WAY
 * earlier.
 */
#if CONFIG_KVM_PIT && TIMER_HZ / CONFIG_HZ < 1
#error Timer tick frequency (CONFIG_HZ) cannot be higher than PIT frequency!
#endif

static __u64 wall_clock_offset;

/*
 * TSC clock specific.
 */

/* TSC at boot; clock reads do not modify the shared origin. */
static __u64 tsc_base;

/* Multiplier for converting TSC ticks to nsecs. (0.32) fixed point. */
static __u32 tsc_mult;

#if !CONFIG_LIBUKINTCTLR_APIC
/*
 * Multiplier for converting nsecs to PIT ticks. (1.32) fixed point.
 *
 * Calculated as:
 *
 *     f = UKARCH_NSEC_PER_SEC / TIMER_HZ   (0.31) fixed point.
 *     pit_mult = 1 / f                     (1.32) fixed point.
 */
static const __u32 pit_mult =
	(1ULL << 63) / ((UKARCH_NSEC_PER_SEC << 31) / TIMER_HZ);
#elif CONFIG_KVM_PIT
#define APIC_TIMER_DIV16 0x3
#define APIC_TIMER_MASKED UK_BIT(16)

static __u32 apic_mult;
#else
#define APIC_TIMER_TSC_DEADLINE (2U << 17)
#define MSR_IA32_TSC_DEADLINE 0x6e0
static __u64 tsc_frequency;
#endif
#if CONFIG_LIBUKINTCTLR_APIC
__uk_pcpuvar bool tscclock_apic_timer_ready;
#endif


/*
 * Read the current i8254 channel 0 tick count.
 */
#if CONFIG_KVM_PIT
static unsigned int i8254_gettick(void)
{
	__u16 rdval;

	uk_arch_x86_64_outb(TIMER_MODE, TIMER_SEL0 | TIMER_LATCH);
	rdval  = uk_arch_x86_64_inb(TIMER_CNTR);
	rdval |= (uk_arch_x86_64_inb(TIMER_CNTR) << 8);
	return rdval;
}

/*
 * Delay for approximately n microseconds using the i8254 channel 0 counter.
 * Timer must be programmed appropriately before calling this function.
 */
static void i8254_delay(unsigned int n)
{
	unsigned int cur_tick, initial_tick;
	int remaining;
	const unsigned long timer_rval = TIMER_HZ / CONFIG_HZ;

	initial_tick = i8254_gettick();

	remaining = (unsigned long long) n * TIMER_HZ / 1000000;

	while (remaining > 1) {
		cur_tick = i8254_gettick();
		if (cur_tick > initial_tick)
			remaining -= timer_rval - (cur_tick - initial_tick);
		else
			remaining -= initial_tick - cur_tick;
		initial_tick = cur_tick;
	}
}

#endif

/*
 * Read a RTC register. Due to PC platform braindead-ness also disables NMI.
 */
static inline __u8 rtc_read(__u8 reg)
{
	uk_arch_x86_64_outb(RTC_COMMAND, reg | RTC_NMI_DISABLE);
	return uk_arch_x86_64_inb(RTC_DATA);
}

/*
 * Return current RTC time. Note that due to waiting for the update cycle to
 * complete, this call may take some time.
 */
static __u64 rtc_gettimeofday(void)
{
	struct uktimeconv_bmkclock dt;
	unsigned long flags;

	flags = uk_lcpu_save_irqf();

	/*
	 * If RTC_UIP is down, we have at least 244us to obtain a
	 * consistent reading before an update can occur.
	 */
	while (rtc_read(RTC_STATUS_A) & RTC_UIP)
		continue;

	dt.dt_sec = uktimeconv_bcdtobin(rtc_read(RTC_SEC));
	dt.dt_min = uktimeconv_bcdtobin(rtc_read(RTC_MIN));
	dt.dt_hour = uktimeconv_bcdtobin(rtc_read(RTC_HOUR));
	dt.dt_day = uktimeconv_bcdtobin(rtc_read(RTC_DAY));
	dt.dt_mon = uktimeconv_bcdtobin(rtc_read(RTC_MONTH));
	dt.dt_year = uktimeconv_bcdtobin(rtc_read(RTC_YEAR)) + 2000;

	uk_lcpu_restore_irqf(flags);

	return uktimeconv_bmkclock_to_nsec(&dt);
}

/*
 * Return monotonic time using TSC clock.
 */
__u64 tscclock_monotonic(void)
{
	__u64 tsc_delta;

	uk_arch_x86_64_rmb();
	tsc_delta = uk_arch_x86_64_rdtsc() - tsc_base;
	if (tsc_delta >= UINT64_MAX / 2)
		return 0;

	return uk_arch_x86_64_mul64_32(tsc_delta, tsc_mult);
}

#if CONFIG_LIBUKINTCTLR_APIC && CONFIG_KVM_PIT
static int apic_timer_calibrate(void)
{
	__u64 start, elapsed, ticks, mult;

	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_LVT_TIMER,
			      APIC_TIMER_MASKED);
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_DCR,
			      APIC_TIMER_DIV16);
	start = tscclock_monotonic();
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_IC, UINT32_MAX);
	i8254_delay(10000);
	ticks = UINT32_MAX -
		uk_arch_x86_64_rdmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_CC);
	elapsed = tscclock_monotonic() - start;
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_IC, 0);
	if (!elapsed || !ticks)
		return -EINVAL;
	mult = (ticks << 32) / elapsed;
	if (!mult || mult > UINT32_MAX)
		return -ERANGE;
	apic_mult = mult;
	return 0;
}
#endif

/* KVM clock ABI: https://docs.kernel.org/virt/kvm/x86/msr.html */
#define MSR_KVM_WALL_CLOCK_NEW 0x4b564d00
#define MSR_KVM_SYSTEM_TIME_NEW 0x4b564d01

struct kvm_clock_info {
	__u32 version;
	__u32 pad0;
	__u64 tsc_timestamp;
	__u64 system_time;
	__u32 multiplier;
	__s8 shift;
	__u8 flags;
	__u8 pad[2];
} __packed;

struct kvm_wall_clock {
	__u32 version;
	__u32 seconds;
	__u32 nanoseconds;
} __packed;

struct kvm_time_sample {
	__u64 frequency;
	__u64 timestamp;
	__u64 epoch;
};

UK_CTASSERT(sizeof(struct kvm_clock_info) == 32);
UK_CTASSERT(sizeof(struct kvm_wall_clock) == 12);

static struct kvm_clock_info kvm_clock __align(32);
static struct kvm_wall_clock kvm_wall __align(4);

static int kvm_read_time(struct kvm_time_sample *sample)
{
	__u32 eax, ebx, ecx, edx, version, multiplier, seconds, nanoseconds;
	__s8 shift;
	__u64 frequency, address, timestamp, system_time, epoch;

	uk_arch_x86_64_cpuid(0x40000000, 0, &eax, &ebx, &ecx, &edx);
	if (eax < 0x40000001 || ebx != 0x4b4d564b ||
	    ecx != 0x564b4d56 || edx != 0x4d)
		return -ENOTSUP;
	uk_arch_x86_64_cpuid(0x40000001, 0, &eax, &ebx, &ecx, &edx);
	if (!(eax & UK_BIT(3)))
		return -ENOTSUP;

	address = uk_paging_virt_to_phys((__vaddr_t)&kvm_clock);
	uk_arch_x86_64_wrmsrl(MSR_KVM_SYSTEM_TIME_NEW, address | 1);
	/* Disable host updates before the kernel can be snapshotted. */
	uk_arch_x86_64_wrmsrl(MSR_KVM_SYSTEM_TIME_NEW, 0);
	uk_arch_x86_64_rmb();
	version = UK_READ_ONCE(kvm_clock.version);
	uk_arch_x86_64_rmb();
	multiplier = UK_READ_ONCE(kvm_clock.multiplier);
	shift = UK_READ_ONCE(kvm_clock.shift);
	timestamp = UK_READ_ONCE(kvm_clock.tsc_timestamp);
	system_time = UK_READ_ONCE(kvm_clock.system_time);
	uk_arch_x86_64_rmb();
	if (!version || (version & 1) ||
	    version != UK_READ_ONCE(kvm_clock.version) || !multiplier ||
	    shift < -32 || shift > 32)
		return -EIO;

	frequency = (UKARCH_NSEC_PER_SEC << 32) / multiplier;
	if (shift < 0) {
		if (frequency > (UINT64_MAX >> -shift))
			return -ERANGE;
		frequency <<= -shift;
	} else {
		frequency >>= shift;
	}
	if (!frequency)
		return -ERANGE;

	address = uk_paging_virt_to_phys((__vaddr_t)&kvm_wall);
	uk_arch_x86_64_wrmsrl(MSR_KVM_WALL_CLOCK_NEW, address);
	uk_arch_x86_64_wrmsrl(MSR_KVM_WALL_CLOCK_NEW, 0);
	uk_arch_x86_64_rmb();
	version = UK_READ_ONCE(kvm_wall.version);
	uk_arch_x86_64_rmb();
	seconds = UK_READ_ONCE(kvm_wall.seconds);
	nanoseconds = UK_READ_ONCE(kvm_wall.nanoseconds);
	uk_arch_x86_64_rmb();
	if (!version || (version & 1) ||
	    version != UK_READ_ONCE(kvm_wall.version) ||
	    nanoseconds >= UKARCH_NSEC_PER_SEC)
		return -EIO;
	epoch = (__u64)seconds * UKARCH_NSEC_PER_SEC + nanoseconds;
	if (system_time > UINT64_MAX - epoch)
		return -ERANGE;
	*sample = (struct kvm_time_sample) {
		.frequency = frequency,
		.timestamp = timestamp,
		.epoch = epoch + system_time
	};
	return 0;
}

/*
 * Calibrate TSC and initialise TSC clock.
 */
int tscclock_init(void)
{
	struct kvm_time_sample sample = { 0 };
	__u64 tsc_freq = 0;
	__u32 eax, ebx, ecx, edx;
	bool have_kvm_clock;
	int rc;

#if CONFIG_KVM_PIT
	/* Initialise i8254 timer channel 0 to mode 2 at CONFIG_HZ frequency */
	uk_arch_x86_64_outb(TIMER_MODE,
			    TIMER_SEL0 | TIMER_RATEGEN | TIMER_16BIT);
	uk_arch_x86_64_outb(TIMER_CNTR, (TIMER_HZ / CONFIG_HZ) & 0xff);
	uk_arch_x86_64_outb(TIMER_CNTR, (TIMER_HZ / CONFIG_HZ) >> 8);

#else
	uk_arch_x86_64_cpuid(1, 0, &eax, &ebx, &ecx, &edx);
	if (!(ecx & UK_BIT(24))) {
		uk_pr_err("PIT-free clock requires TSC-deadline support\n");
		return -ENODEV;
	}
#endif

	tsc_base = uk_arch_x86_64_rdtsc();
	rc = kvm_read_time(&sample);
	have_kvm_clock = !rc;
	if (have_kvm_clock) {
		tsc_base = sample.timestamp;
		tsc_freq = sample.frequency;
	} else if (rc != -ENOTSUP) {
		return rc;
	}

	/*
	 * Attempt to retrieve TSC frequency via the hypervisor generic cpuid
	 * timing information leaf. 0x40000010 returns the (virtual) TSC
	 * frequency in kHz, or 0 if the feature is not supported by the
	 * hypervisor.
	 */
	uk_arch_x86_64_cpuid(0x40000000, 0, &eax, &ebx, &ecx, &edx);
	if (!tsc_freq && eax >= 0x40000010) {
		uk_pr_info("Retrieving TSC clock frequency from hypervisor\n");
		tsc_base = uk_arch_x86_64_rdtsc();
		uk_arch_x86_64_cpuid(0x40000010, 0, &eax, &ebx, &ecx, &edx);
		tsc_freq = (__u64)eax * 1000;
	}

#if CONFIG_KVM_PIT
	/*
	 * If we could not retrieve the TSC frequency from the hypervisor,
	 * calibrate against an 0.1s delay using the i8254 timer. This is
	 * undesirable as it delays the boot sequence.
	 */
	if (!tsc_freq) {
		uk_pr_info("Calibrating TSC clock against i8254 timer\n");
		tsc_base = uk_arch_x86_64_rdtsc();
		i8254_delay(100000);
		tsc_freq = (uk_arch_x86_64_rdtsc() - tsc_base) * 10;
	}
#endif
	if (!tsc_freq) {
		uk_pr_err("No published TSC frequency and PIT fallback is disabled\n");
		return -ENODEV;
	}
	if (tsc_freq > (UKARCH_NSEC_PER_SEC << 32) ||
	    (UKARCH_NSEC_PER_SEC << 32) / tsc_freq > UINT32_MAX)
		return -ERANGE;
#if !CONFIG_KVM_PIT
	tsc_frequency = tsc_freq;
#endif

	/*
	 * Calculate TSC scaling multiplier.
	 *
	 * (0.32) tsc_mult = UKARCH_NSEC_PER_SEC (32.32) / tsc_freq (32.0)
	 *
	 * Warning, do not print anything between TSC calibration and the
	 * setting of tsc_mult, as the clock is not usable until then.

	 */
	tsc_mult = (UKARCH_NSEC_PER_SEC << 32) / tsc_freq;

	uk_pr_info("Clock source: TSC, frequency estimate is %llu Hz\n",
		   (unsigned long long) tsc_freq);

	if (have_kvm_clock)
		wall_clock_offset = sample.epoch;
	else
		wall_clock_offset = rtc_gettimeofday() - tscclock_monotonic();

#if CONFIG_LIBUKINTCTLR_APIC && CONFIG_KVM_PIT
	rc = apic_timer_calibrate();

	if (rc)
		return rc;
#endif

#if CONFIG_KVM_PIT
	/*
	 * Initialise i8254 timer channel 0 to mode 4 (one shot).
	 */
	uk_arch_x86_64_outb(TIMER_MODE,
			    TIMER_SEL0 | TIMER_ONESHOT | TIMER_16BIT);
	uk_arch_x86_64_outb(TIMER_CNTR, 0);
	uk_arch_x86_64_outb(TIMER_CNTR, 0);
#endif

	return 0;
}

/*
 * Return epoch offset (wall time offset to monotonic clock start).
 */
__u64 tscclock_epochoffset(void)
{
	return wall_clock_offset;
}

#if !CONFIG_KVM_PIT
void ukplat_time_arm_timer(__nsec until)
{
	__u64 deadline;

	UK_ASSERT(uk_lcpu_irqs_disabled());
	if (!uk_pcpuvar_current_get(tscclock_apic_timer_ready)) {
		uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_LVT_TIMER,
				      APIC_TIMER_TSC_DEADLINE |
				      (32 + ukplat_time_get_irq()));
		uk_pcpuvar_current_set(tscclock_apic_timer_ready, true);
	}
	until = MAX(until, ukplat_monotonic_clock() + 1);
	deadline = tsc_base + (until / UKARCH_NSEC_PER_SEC) * tsc_frequency;
	until %= UKARCH_NSEC_PER_SEC;
	deadline += until * (tsc_frequency / UKARCH_NSEC_PER_SEC) +
		until * (tsc_frequency % UKARCH_NSEC_PER_SEC) /
		UKARCH_NSEC_PER_SEC;
	uk_arch_x86_64_wrmsrl(MSR_IA32_TSC_DEADLINE, deadline);
}
#elif CONFIG_LIBUKINTCTLR_APIC
void ukplat_time_arm_timer(__nsec until)
{
	__u64 now, ticks;

	UK_ASSERT(uk_lcpu_irqs_disabled());
	if (!uk_pcpuvar_current_get(tscclock_apic_timer_ready)) {
		uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_DCR,
				      APIC_TIMER_DIV16);
		uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_LVT_TIMER,
				      32 + ukplat_time_get_irq());
		uk_pcpuvar_current_set(tscclock_apic_timer_ready, true);
	}
	now = ukplat_monotonic_clock();
	until = MAX(until, now + 1);
	ticks = uk_arch_x86_64_mul64_32(until - now, apic_mult);
	ticks = MIN(ticks + 1, UINT32_MAX);
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_IC, ticks);
}
#else
/*
 * Minimum delta to sleep using PIT. Programming seems to have an overhead of
 * 3-4us, but play it safe here.
 */
#define PIT_MIN_DELTA	16

/*
 * Returns early if any interrupts are serviced, or if the requested delay is
 * too short. Must be called with interrupts disabled, will enable interrupts
 * "atomically" during idle loop.
 *
 * This function must be called only from the scheduler. It will screw
 * your system if you do otherwise. And, there is no reason you
 * actually want to use it anywhere else. THIS IS NOT A YIELD or any
 * kind of mutex_lock. It will simply halt the cpu, not allowing any
 * other thread to execute.
 */
static void tscclock_cpu_block(__u64 until)
{
	__u64 now, delta_ns;
	__u64 delta_ticks;
	unsigned int ticks;

	UK_ASSERT(uk_lcpu_irqs_disabled());

	now = ukplat_monotonic_clock();

	/*
	 * Compute delta in PIT ticks. Return if it is less than minimum safe
	 * amount of ticks.  Essentially this will cause us to spin until
	 * the timeout.
	 */
	delta_ns = until - now;
	delta_ticks = uk_arch_x86_64_mul64_32(delta_ns, pit_mult);
	if (delta_ticks < PIT_MIN_DELTA) {
		/*
		 * Since we are "spinning", quickly enable interrupts in
		 * the hopes that we might get new work and can do something
		 * else than spin.
		 */
		uk_lcpu_enable_irq();
		uk_arch_x86_64_nop(); /* ints are enabled 1 instr after sti */
		uk_lcpu_disable_irq();
		return;
	}

	/*
	 * Program the timer to interrupt the CPU after the delay has expired.
	 * Maximum timer delay is 65535 ticks.
	 */
	if (delta_ticks > 65535)
		ticks = 65535;
	else
		ticks = delta_ticks;

	/*
	 * Note that according to the Intel 82C54 datasheet, p12 the
	 * interrupt is actually delivered in N + 1 ticks.
	 */
	ticks -= 1;
	uk_arch_x86_64_outb(TIMER_CNTR, ticks & 0xff);
	uk_arch_x86_64_outb(TIMER_CNTR, ticks >> 8);

	/*
	 * Wait for any interrupt. If we got an interrupt then just
	 * return into the scheduler (this func is called _ONLY_ from
	 * a scheduler, see the note above) which will check if there
	 * is work to do and call us again here if not.
	 *
	 * TODO: It would be more efficient for longer sleeps to be
	 * able to distinguish if the interrupt was the PIT interrupt
	 * and no other, but this will do for now.
	 */
	uk_lcpu_halt_irq();
}
#endif

#if !CONFIG_KVM_PIT || CONFIG_LIBUKINTCTLR_APIC
static void tscclock_cpu_block(__u64 until)
{
	UK_ASSERT(uk_lcpu_irqs_disabled());
	if (ukplat_monotonic_clock() >= until)
		return;
	ukplat_time_arm_timer(until);
	uk_lcpu_halt_irq();
#if !CONFIG_KVM_PIT
	uk_arch_x86_64_wrmsrl(MSR_IA32_TSC_DEADLINE, 0);
#else
	uk_arch_x86_64_wrmsrl(UK_ARCH_X86_64_APIC_MSR_TIMER_IC, 0);
#endif
}
#endif

__uk_pcpuvar unsigned long sched_have_pending_events;

void time_block_until(__snsec until)
{
	while ((__snsec) ukplat_monotonic_clock() < until) {
		if (uk_and_relax(uk_pcpuvar_current_ptr_get(sched_have_pending_events), 0))
			break;
		tscclock_cpu_block(until);

	}
}
