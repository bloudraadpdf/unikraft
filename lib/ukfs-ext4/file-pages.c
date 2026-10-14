/* SPDX-License-Identifier: BSD-3-Clause */
#include "file-pages.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <ext4_inode.h>
#include <uk/pod.h>
#include <uk/pod/anon.h>

static void page_free(void *addr, __u64 offset, __u32 count, void *arg __unused)
{
    int result __maybe_unused = uk_pod_default_free(addr, count,
                                  &uk_pod_anon_ops, NULL, offset);
    UK_ASSERT(!result);
}

static void *page_load(__u64 offset, __u32 count, void *arg)
{
    struct ext4_inode_ref *inode = arg;
    size_t done;
    void *buffer = uk_pod_default_alloc(count, 1, &uk_pod_anon_ops, NULL, offset);
    if (PTRISERR(buffer))
        return buffer;
    int result = ukext4_inode_read(inode, buffer,
                    (size_t)count * UK_PAGING_PAGE_SIZE,
                    offset * UK_PAGING_PAGE_SIZE, &done);
    if (result) {
        page_free(buffer, offset, count, NULL);
        return ERR2PTR(-result);
    }
    return buffer;
}

static const struct uk_sparsebuf_alloc_funcs page_ops = {
    .alloc_new = page_load,
    .alloc_free = page_free
};

static struct uk_sparsebuf_ctx page_context(struct ext4_inode_ref *inode)
{
    return (struct uk_sparsebuf_ctx) {
        .funcs = &page_ops, .alloc = uk_alloc_get_default(), .arg = inode
    };
}

static size_t page_at(struct ukext4_pages *pages, size_t offset, char **buffer)
{
    struct uk_sparsebuf_cur cursor;
    *buffer = NULL;
    if (!uk_sparsebuf_lookup(&pages->head, offset / UK_PAGING_PAGE_SIZE, &cursor))
        return 0;
    return uk_sparsebuf_memat(offset, &cursor, buffer);
}

ssize_t ukext4_pages_mem(struct ukext4_pages *pages, struct ext4_inode_ref *inode,
                        enum uk_file_mem_op op, size_t offset, size_t length,
                        struct iovec *iov, size_t count)
{
    struct uk_sparsebuf_ctx context = page_context(inode);
    const size_t size = ext4_inode_get_size(&inode->fs->sb, inode->inode);
    if (length > SIZE_MAX - offset)
        return -EINVAL;
    if (op == UKFILE_MEM_GIFT)
        return -EOPNOTSUPP;
    if (!length || offset >= size)
        return 0;
    if (op == UKFILE_MEM_ACQUIRE) {
        if (offset % UK_PAGING_PAGE_SIZE || length % UK_PAGING_PAGE_SIZE)
            return -EINVAL;
        ssize_t result = uk_sparsebuf_fill(&context, &pages->head,
                            offset / UK_PAGING_PAGE_SIZE, 1, 1);
        return result < 0 ? result : result * (ssize_t)UK_PAGING_PAGE_SIZE;
    }
    length = MIN(length, size - offset);
    size_t total = 0;
    while (length && (op == UKFILE_MEM_BUFCOUNT || total < count)) {
        char *buffer;
        size_t bytes = page_at(pages, offset, &buffer);
        if (!bytes || !buffer)
            return total ? (ssize_t)total : -ENOENT;
        bytes = MIN(bytes, length);
        if (op != UKFILE_MEM_BUFCOUNT)
            iov[total] = (struct iovec) { .iov_base = buffer, .iov_len = bytes };
        total++;
        offset += bytes;
        length -= bytes;
    }
    return total;
}

int ukext4_pages_io(struct ukext4_pages *pages, struct ext4_inode_ref *inode,
                    void *buffer, size_t length, size_t offset, size_t *done, int write)
{
    const uint64_t size = ext4_inode_get_size(&inode->fs->sb, inode->inode);
    *done = 0;
    if (!write) {
        if (offset >= size)
            return 0;
        length = MIN(length, size - offset);
    }
    while (*done < length) {
        char *cached;
        size_t bytes = page_at(pages, offset, &cached);
        size_t transferred = 0;
        int result = 0;
        bytes = bytes ? MIN(bytes, length - *done) : length - *done;
        if (write) {
            result = ukext4_inode_write(inode, (char *)buffer + *done,
                                        bytes, offset, &transferred);
            if (cached && transferred)
                memmove(cached, (char *)buffer + *done, transferred);
        } else if (cached) {
            memmove((char *)buffer + *done, cached, bytes);
            transferred = bytes;
        } else {
            result = ukext4_inode_read(inode, (char *)buffer + *done,
                                       bytes, offset, &transferred);
        }
        *done += transferred;
        offset += transferred;
        if (result || transferred < bytes)
            return result;
    }
    return 0;
}

int ukext4_pages_writeback(struct ukext4_pages *pages, struct ext4_inode_ref *inode,
                           int *changed)
{
    struct uk_sparsebuf_cur cursor;
    const size_t size = ext4_inode_get_size(&inode->fs->sb, inode->inode);
    *changed = 0;
    if (!uk_sparsebuf_lookup(&pages->head, 0, &cursor))
        return 0;
    char *scratch = malloc(2 * UK_PAGING_PAGE_SIZE);
    if (!scratch)
        return ENOMEM;
    int result = 0;
    UK_SPARSEBUF_FOREACH(&pages->head, &cursor) {
        struct uk_sparsebuf_slice *slice = uk_sparsebuf_slice_at(&cursor);
        size_t offset = uk_sparsebuf_slice_offset(slice);
        for (size_t index = 0; index < uk_sparsebuf_slice_size(slice) && offset < size;
             index += UK_PAGING_PAGE_SIZE, offset += UK_PAGING_PAGE_SIZE) {
            size_t length = MIN(UK_PAGING_PAGE_SIZE, size - offset), done;
            memcpy(scratch, slice->buf + index, length);
            result = ukext4_inode_read(inode, scratch + UK_PAGING_PAGE_SIZE,
                                       length, offset, &done);
            if (!result && done != length)
                result = EIO;
            if (result)
                goto out;
            if (!memcmp(scratch, scratch + UK_PAGING_PAGE_SIZE, length))
                continue;
            result = ukext4_inode_write(inode, scratch, length, offset, &done);
            *changed |= done != 0;
            if (!result && done != length)
                result = EIO;
            if (result)
                goto out;
        }
    }
out:
    free(scratch);
    return result;
}

void ukext4_pages_zero(struct ukext4_pages *pages, size_t size)
{
    struct uk_sparsebuf_cur cursor;
    UK_SPARSEBUF_FOREACH(&pages->head, &cursor) {
        struct uk_sparsebuf_slice *slice = uk_sparsebuf_slice_at(&cursor);
        const size_t offset = uk_sparsebuf_slice_offset(slice);
        const size_t length = uk_sparsebuf_slice_size(slice);
        const size_t start = size > offset ? MIN(size - offset, length) : 0;
        memset(slice->buf + start, 0, length - start);
    }
}

void ukext4_pages_clear(struct ukext4_pages *pages)
{
    struct uk_sparsebuf_ctx context = page_context(NULL);
    uk_sparsebuf_clear(&context, &pages->head);
}

void ukext4_pages_release(struct ukext4_pages *pages, size_t offset, size_t length, size_t size)
{
    struct uk_sparsebuf_ctx context = page_context(NULL);
    const size_t first = offset / UK_PAGING_PAGE_SIZE;
    const size_t end = DIV_ROUND_UP(offset + length, UK_PAGING_PAGE_SIZE);
    const size_t split = MIN(MAX(DIV_ROUND_UP(size, UK_PAGING_PAGE_SIZE), first), end);
    int result = 0;
    if (split > first)
        result = uk_sparsebuf_ref_release(&context, &pages->head, first, split - first, 0);
    if (!result && end > split)
        result = uk_sparsebuf_ref_release(&context, &pages->head, split, end - split, 1);
    UK_ASSERT(!result);
}
