# ext4 filesystem components

This directory contains the BSD-licensed headers and runtime components
selected from lwext4. Each imported source retains its copyright notice,
licence terms and disclaimer. `../sources.json` records the upstream
revision, archive digest and imported file digests.

The upstream `src/ext4_extent.c` and `src/ext4_xattr.c` implementations are
not imported. Their GPL-2.0-or-later notices do not meet this consumer's
licence requirement. Their BSD-licensed interface headers are retained.

The extent and extended-attribute replacements are imported from FreeBSD.
Their source files retain the BSD licence notices. `../sources.json` records
the FreeBSD revision, original paths and file digests. The extent and attribute
code uses the retained block, allocator and journal interfaces. Attribute
block checksums also follow the pinned FreeBSD checksum implementation.

The native Linux extent check uses a real ext4 image and journal. It covers
sparse multilevel trees, contiguous extents, partial removal, conversion
of unwritten preallocated blocks, and inode and external attributes. Each
stage closes the filesystem and runs `e2fsck`. Run it with an unused build
directory:

```sh
make -f lib/ukfs-ext4/tests/Makefile CC=gcc BUILD="$PWD/build/ext4-tests" test
```

The `test-seeded` target runs the same checks and journal recovery with
`metadata_csum_seed`. It changes the image UUID before the checks, retaining
the stored checksum seed. Use a separate build directory for this target.

The optional `shared-attributes` target requires Linux mount privileges. It
uses Linux to create three files that share an attribute block, changes one
file through this implementation, and deletes the other two. It checks the
image with `e2fsck` and reads the retained value through Linux.

The `recovery` target interrupts a write before its commit record or before
its inode reaches the home block, then checks recovery of an extent and an
external attribute. `recovery-linux` checks the same cases through a Linux
mount and requires mount privileges. Both targets use the image created by
`test` and reject filesystem repair prompts. Use `BLOCK_SIZE=4096` with a
separate build directory to check the other superblock layout.

Journal checksum encoding follows the
[JBD2 format](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html).
Transactions include changed filesystem superblocks, including allocation
counters. Shutdown retains the next transaction number so stale commit
records cannot validate later incomplete writes.

The interruption checks persist earlier writes before process exit. They
do not simulate device write reordering or power loss. Guest integration
and snapshot checks belong to the consumer. Values stored in separate
attribute inodes are unsupported; the filesystem does not admit `ea_inode`.
