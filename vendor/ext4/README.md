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

The optional `shared-attributes` target requires Linux mount privileges. It
uses Linux to create three files that share an attribute block, changes one
file through this implementation, and deletes the other two. It checks the
image with `e2fsck` and reads the retained value through Linux.

These native checks do not qualify guest integration, snapshot restore or
interrupted-write recovery. Values stored in separate attribute inodes are
unsupported; the filesystem does not admit the `ea_inode` feature.
