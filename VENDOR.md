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
after replay. Managed shim checks remain unfinished.

ARM64 and AMD64 metadata layouts, snapshot discovery and entropy setup are
selected by architecture. AMD64 has not yet been built from the combined
vendor tree.

Tokoloshe still consumes its original patch set. Move its source pin and
remove those patch files together after the complete fork passes the guest
integration checks.
