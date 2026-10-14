# ext4 filesystem components

This directory contains the BSD-licensed headers and runtime components
selected from lwext4. Each imported source retains its copyright notice,
licence terms and disclaimer. `../sources.json` records the upstream
revision, archive digest and imported file digests.

The upstream `src/ext4_extent.c` and `src/ext4_xattr.c` implementations are
not imported. Their GPL-2.0-or-later notices do not meet this consumer's
licence requirement. Their BSD-licensed interface headers are retained.

Extent and extended-attribute replacements are required before this
filesystem can be built. The retained journal and transaction interfaces
must cover changes made by those replacements.
