# Tokoloshe vendor branch

`patch/0.21.0` carries the Tokoloshe changes on upstream Unikraft commit
`b7d7de873778824104206bb3104987fe09936418`. Upstream branches and tags remain
unchanged. Follow the [fork standard](https://github.com/bloudraadpdf/mzansi/blob/main/docs/reference/forks.md).

The authoritative repository is
`ssh://git@git.bloudraad.io/external/unikernel.git`. The GitHub backup is
`bloudraadpdf/unikraft`.

## Source ownership

Kernel changes are source commits above the upstream base. Their commit
bodies identify the consumer, original Tokoloshe commit and upstream status.
The initial combined kernel patch is split by component.

The [vendor manifest](vendor/sources.json) records exact dependency revisions
and archive digests. Imports retain upstream copyright and licence notices.
Local dependency changes follow their imports as separate source commits.

| Directory | Purpose |
|---|---|
| `vendor/app-elfloader` | Load application ELF files |
| `vendor/lib-libelf` | Read ELF metadata |
| `vendor/lib-lwip` | Adapt lwIP to Unikraft |
| `vendor/lwip-source` | TCP/IP stack, including upstream adapter changes |
| `vendor/erofs-utils` | Read EROFS application images |
| `vendor/lz4` | Decompress EROFS data |
| `vendor/ext4` | Selected BSD filesystem components |

The [ext4 import notes](vendor/ext4/README.md) identify the excluded GPL
implementations. The FreeBSD extent and attribute ports pass the native ARM64
filesystem checks, including shared attributes created by Linux. The ARM64
kernel builds with `CONFIG_LIBUKFS_EXT4=y` and passes the guest VFS probe,
including sparse writes above 64 GiB, ownership, links, rename and unmount.
Its output disk passes `e2fsck`.

## Build and verification status

Tokoloshe owns the guest configurations, bootstraps and integration checks.
Use `vendor/app-elfloader` for the Unikraft `A` build argument and
`vendor/lib-libelf:vendor/lib-lwip` for `L`, with absolute paths. EROFS, LZ4
and lwIP sources come from this tree; the build does not patch those sources.

The migrated source compiles on the ARM64 appliance with the full Tokoloshe
configuration. A two-vCPU SQLite probe captures an open read transaction,
then restores two VMs. Each accepts a new vsock connection and commits its
own marker. Both disks pass `e2fsck` and SQLite integrity checks; the captured
source disk stays unchanged. Native interrupted-write checks pass on 1 KiB
and 4 KiB images through both the port and Linux recovery. They cover a
committed allocation and an aborted transaction, with filesystem checks
after replay. Managed ARM64 shim checks pass for EROFS and CPIO roots with
two CPUs, four requests across reused and replaced VMs, writable scratch
contents, directory ownership and cleanup. CPIO extraction restores archive
UID/GID values without following symbolic links.

ARM64 and AMD64 metadata layouts, snapshot discovery and entropy setup are
selected by architecture. The AMD64 vendor kernel builds with one or eight
CPU slots. Application checks pass with 1, 2, 4 and 8 CPUs. Each CPU reads
random bytes and the monotonic clock, faults an anonymous page, and has its
own interrupt descriptor table. An eight-CPU snapshot resumes the waiting
threads after a new vsock connection and completes the same checks. The
two-CPU filesystem probe verifies 32 records and directory operations.

AMD64 secondary CPUs load the active page table and initialise their syscall
registers before application execution. Spinlock acquisition and release
constrain compiler memory ordering; a missing acquisition barrier previously
allowed thread flags to be read before the scheduler lock.

Tokoloshe pins this repository and exports its source in release archives.
Its guest feature contracts and architecture-specific acceptance checks
remain the authority for supported launches.

## Optional timer support

`CONFIG_KVM_PIT` keeps the legacy AMD64 PIT calibration and timer fallback
available. It defaults to enabled for upstream configurations. Disabling it
selects the local APIC and compiles out PIT port access and calibration.
The kernel then requires TSC-deadline support and a published TSC frequency.
KVM clock timing data supplies the frequency without a calibration delay;
the temporary timing page is disabled before snapshots can be taken.
The existing generic hypervisor frequency leaf remains available as a
fallback. ARM64 continues to use its architectural timer.

## Optional SMP support

`CONFIG_UKPLAT_CPU_MAXCOUNT=1` disables `CONFIG_HAVE_SMP` on both
architectures. AMD64 excludes the 16-bit secondary startup trampoline in
this configuration; the common primary CPU entry remains available.

## C library ABI checks

The ELF interpreter path selects `glibc` or `musl` for each application.
They use one kernel binary and the same Linux syscall handler.
Tokoloshe owns the pinned `libc-test` runner, Linux controls, result digests
and operation limits in `conformance/unikraft/libc`.

Application entry clears the signal mask inherited from PID 1. The signal
mask syscall preserves overlapping input and output. Futex waits validate
and convert their clock deadlines, support requeue operations and wake for
pending signals. Waiter removal follows the waiter node after a requeue.
`waitid` accepts a null output buffer with the Linux return value.
`nanosleep` registers pending-signal wake-ups, preserves the remaining
duration on interruption and rejects negative seconds.

The ELF loader returns the auxiliary vector on the new application stack.
Each process VM context retains its own vector for `PR_GET_AUXV` and releases
it with the context. `AT_EXECFN` and `AT_RANDOM` data are copied to the new
stack before the caller stack is released. Other `prctl` operations return
`EINVAL` rather than reporting success without an implementation.

Tokoloshe retains the initial one-CPU AMD64 and ARM64 C library results
and their remaining compatibility gaps. Each architecture passes the 26
initial signal, futex, wait, auxiliary-vector and dynamic TLS probes with
static and dynamic application linking. These checks do not establish full
C library or Linux compatibility, process memory protection or snapshot
acceptance.

Robust futex lists are registered per thread. Thread exit marks owned locks
with `FUTEX_OWNER_DIED` and wakes waiters before clearing the child TID.
Fault-safe reads and atomic compare-exchange stop cleanup at inaccessible
list entries or lock words. The Tokoloshe `robust-owner` probe and upstream
detached-thread regression pass with both C libraries and linking modes on
AMD64 and ARM64. Priority-inheritance futex operations remain separate work.

`CONFIG_LIBPOSIX_PROCESS_PREEMPT` uses the platform's local one-shot timer
to schedule binary application threads at 10 ms intervals. Kernel code
remains cooperative. The interrupt return path preserves general, floating
point and TLS registers before it calls the scheduler. Each kernel thread
object retains its deadline across application syscalls. It does not read
application TLS as kernel state. Application entry and signal delivery use
the same return path. Tokoloshe's cancellation, register-context and signal
origin probes pass with both libraries and linking modes on AMD64 and ARM64.

`CONFIG_LIBPOSIX_PROCESS_SYSV_SHM` provides keyed shared-memory segments,
anonymous RAMfs backing and distinct shared mappings. Mapping lifetime
callbacks track splits, partial unmaps, protection changes and final removal.
VM-context release detaches mappings on exec and exit. It receives the
exiting PID explicitly because cleanup can run on a kernel thread.
Tokoloshe's shared-memory and mapping probes pass all eight library/linking
combinations per architecture, including a spawned child's attachment,
implicit exit cleanup, delayed removal and stale-ID rejection. Huge-page
segments return `EOPNOTSUPP`; the allocator has no huge-page pool.
