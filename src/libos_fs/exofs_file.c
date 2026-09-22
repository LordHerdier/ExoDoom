/*
 * exofs_file.c — file operations (SCRUM-189).
 *
 * The cfat equivalents are in file.c, but only loosely: half that file is
 * import/export between the filesystem and a host FILE*, which has no
 * meaning here, and the other half reaches into the mmap'd image with raw
 * pointers. What carries over is the model — a file is a FAT chain of blocks
 * plus a byte count in its directory entry — and nothing of the code.
 *
 * Three things are worth knowing before editing:
 *
 *   AN EMPTY FILE HAS NO CHAIN. first_block is EXOFS_NO_BLOCK until the
 *   first byte is written, so a volume full of empty files costs no data
 *   blocks. Every path below has to cope with that, which is why
 *   ensure_block() and not exofs_chain_nth() is what writing goes through.
 *
 *   THE CHAIN POSITION IS CACHED. Finding the block holding offset N means
 *   walking N/512 links. Re-walking from the head on every call makes
 *   sequential I/O O(blocks²) — for SCRUM-190's ~28 MB IWAD that is ~57000
 *   steps by the end of the file, per call. The handle remembers the block
 *   it last touched and its index, so a forward step is one link.
 *
 *   HOLES ARE FREE. Seeking past the end and writing leaves a gap that
 *   reads as zeros, with no hole tracking anywhere: every block is zeroed
 *   when allocated (docs/filesystem.md §1.1), so unwritten bytes inside an
 *   allocated block are already zero.
 */

#include "exofs.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs_dirent.h"
#include "exofs_path.h"
#include "exofs_name.h"
#include "exofs_fat.h"
#include "exofs_blockdev.h"

#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* ---- Chain navigation ---------------------------------------------------
 *
 * block_at() finds the block holding chain index `want`, using and updating
 * the handle's cached position. ensure_block() does the same but allocates
 * whatever is missing.
 */

static int block_at(exofs_volume_t *v, exofs_file_t *f, uint32_t want,
                    uint32_t *out)
{
    if (f->first_block == EXOFS_NO_BLOCK) return -EXO_EINVAL;

    uint32_t cur, idx;

    /* Resume from the cache when it is at or before the target; otherwise
     * start over from the head. Only a backwards seek pays for a re-walk. */
    if (f->cur_block != EXOFS_NO_BLOCK && f->cur_index <= want) {
        cur = f->cur_block;
        idx = f->cur_index;
    } else {
        cur = f->first_block;
        idx = 0;
    }

    while (idx < want) {
        uint32_t next;
        int rc = exofs_fat_get(v, cur, &next);
        if (rc < 0) return rc;

        if (next == EXOFS_BLOCK_EOC)  return -EXO_EINVAL;  /* past the end */
        if (next == EXOFS_BLOCK_FREE) return -EXO_EIO;     /* corrupt      */

        cur = next;
        idx++;

        /* Bound the walk the same way exofs_fat.c does: a chain cannot be
         * longer than the volume. */
        if (idx > v->total_blocks) return -EXO_EIO;
    }

    f->cur_block = cur;
    f->cur_index = idx;
    *out = cur;
    return 0;
}

static int ensure_block(exofs_volume_t *v, exofs_file_t *f, uint32_t want,
                        uint32_t *out)
{
    /* First block of a file that has none yet. */
    if (f->first_block == EXOFS_NO_BLOCK) {
        uint32_t head;
        int rc = exofs_fat_alloc(v, &head);
        if (rc < 0) return rc;

        f->first_block = head;
        f->cur_block   = head;
        f->cur_index   = 0;
        f->dirty       = 1;
    }

    for (;;) {
        int rc = block_at(v, f, want, out);
        if (rc == 0) return 0;
        if (rc != -EXO_EINVAL) return rc;

        /* Short by at least one block: extend and try again. Extending from
         * the tail rather than from the head keeps this linear overall,
         * since exofs_chain_extend() walks to the end itself. */
        uint32_t fresh;
        rc = exofs_chain_extend(v, f->first_block, &fresh);
        if (rc < 0) return rc;
    }
}

/* ---- Metadata writeback ------------------------------------------------- */

static int flush_metadata(exofs_volume_t *v, exofs_file_t *f)
{
    if (!f->dirty) return 0;

    exofs_entry_ref_t ref = { f->ent_block, f->ent_index };
    exofs_dirent_t e;

    int rc = exofs_dirent_read(v, &ref, &e);
    if (rc < 0) return rc;

    e.first_block = f->first_block;
    e.size        = f->size;
    e.mtime       = exofs_bdev_ticks();

    rc = exofs_dirent_write(v, &ref, &e);
    if (rc < 0) return rc;

    f->dirty = 0;
    return 0;
}

int exofs_fflush(exofs_file_t *f)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || f == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;

    int rc = flush_metadata(v, f);
    if (rc < 0) return rc;

    return exofs_sync();
}

int exofs_close(exofs_file_t *f)
{
    if (f == NULL) return -EXO_EINVAL;
    if (!f->open) return 0;

    int rc = exofs_fflush(f);
    f->open = 0;
    return rc;
}

/* ---- Open --------------------------------------------------------------- */

int exofs_open(const char *path, uint32_t flags, exofs_file_t *out)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    /* A handle that can do neither is a caller bug, not a no-op handle. */
    if ((flags & (EXOFS_O_READ | EXOFS_O_WRITE)) == 0) return -EXO_EINVAL;

    exofs_entry_ref_t ref;
    exofs_dirent_t e;

    int rc = exofs_path_resolve(v, path, &ref, &e);

    if (rc == -EXO_ENOENT && (flags & EXOFS_O_CREATE)) {
        exofs_dirent_t parent;
        char leaf[EXOFS_MAX_NAME + 1];

        rc = exofs_path_resolve_parent(v, path, &parent, leaf);
        if (rc < 0) return rc;

        rc = exofs_dir_add(v, parent.first_block, leaf, EXOFS_ATTR_FILE,
                           EXOFS_NO_BLOCK, 0, &ref);
        if (rc < 0) return rc;

        rc = exofs_dirent_read(v, &ref, &e);
        if (rc < 0) return rc;
    }
    if (rc < 0) return rc;

    if (e.attributes & EXOFS_ATTR_DIRECTORY) return -EXO_EISDIR;

    /* The root is the only entry with no slot behind it, and it is a
     * directory, so the check above has already rejected it — but a file
     * handle with nowhere to flush to would be silently unwritable, so this
     * is asserted rather than assumed. */
    if (ref.block == EXOFS_NO_BLOCK) return -EXO_EISDIR;

    memset(out, 0, sizeof(*out));
    out->open        = 1;
    out->flags       = flags;
    out->ent_block   = ref.block;
    out->ent_index   = ref.index;
    out->first_block = e.first_block;
    out->size        = e.size;
    out->pos         = 0;
    out->dirty       = 0;
    out->cur_block   = EXOFS_NO_BLOCK;
    out->cur_index   = 0;

    if ((flags & EXOFS_O_TRUNC) && out->first_block != EXOFS_NO_BLOCK) {
        rc = exofs_chain_free(v, out->first_block);
        if (rc < 0) return rc;

        out->first_block = EXOFS_NO_BLOCK;
        out->size        = 0;
        out->cur_block   = EXOFS_NO_BLOCK;
        out->dirty       = 1;

        /* Record the truncation immediately. Deferring it would leave the
         * entry naming a chain that is already back on the free list — the
         * dangling case docs/filesystem.md §1 exists to avoid, and the one
         * place in the file layer where a deferred metadata write would be
         * unsafe rather than merely lossy. */
        rc = flush_metadata(v, out);
        if (rc < 0) return rc;

        rc = exofs_sync();
        if (rc < 0) return rc;
    }

    if (flags & EXOFS_O_APPEND) out->pos = out->size;

    return 0;
}

/* ---- Read / write ------------------------------------------------------- */

int64_t exofs_read(exofs_file_t *f, void *buf, uint32_t n)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || f == NULL || buf == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;
    if (!(f->flags & EXOFS_O_READ)) return -EXO_EACCES;

    if (f->pos >= f->size || n == 0) return 0;

    /* Clamp to what is actually there: a short read at EOF, never a read of
     * whatever the last block happens to still hold past the size. */
    uint32_t avail = f->size - f->pos;
    if (n > avail) n = avail;

    uint8_t *block = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (block == NULL) return -EXO_ENOMEM;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    int rc = 0;

    while (done < n) {
        uint32_t idx = f->pos / EXOFS_BLOCK_SIZE;
        uint32_t off = f->pos % EXOFS_BLOCK_SIZE;
        uint32_t take = EXOFS_BLOCK_SIZE - off;
        if (take > n - done) take = n - done;

        uint32_t blk;
        rc = block_at(v, f, idx, &blk);
        if (rc < 0) goto out;

        rc = exofs_read_block(blk, block);
        if (rc < 0) goto out;

        memcpy(dst + done, block + off, take);
        done    += take;
        f->pos  += take;
    }

    rc = 0;

out:
    libos_heap_free(block);
    return (rc < 0) ? rc : (int64_t)done;
}

int64_t exofs_write(exofs_file_t *f, const void *buf, uint32_t n)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || f == NULL || buf == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;
    if (!(f->flags & EXOFS_O_WRITE)) return -EXO_EACCES;

    if (n == 0) return 0;

    if (f->flags & EXOFS_O_APPEND) f->pos = f->size;

    /* A file's size is a uint32_t on disk, so refuse rather than wrap. */
    if (n > 0xFFFFFFFFu - f->pos) return -EXO_EFBIG;

    uint8_t *block = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (block == NULL) return -EXO_ENOMEM;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    int rc = 0;

    while (done < n) {
        uint32_t idx = f->pos / EXOFS_BLOCK_SIZE;
        uint32_t off = f->pos % EXOFS_BLOCK_SIZE;
        uint32_t put = EXOFS_BLOCK_SIZE - off;
        if (put > n - done) put = n - done;

        uint32_t blk;
        rc = ensure_block(v, f, idx, &blk);
        if (rc < 0) goto out;

        if (put == EXOFS_BLOCK_SIZE) {
            /* Whole block: no need to read what is about to be replaced. */
            memcpy(block, src + done, put);
        } else {
            rc = exofs_read_block(blk, block);
            if (rc < 0) goto out;
            memcpy(block + off, src + done, put);
        }

        rc = exofs_write_block(blk, block);
        if (rc < 0) goto out;

        done   += put;
        f->pos += put;

        if (f->pos > f->size) {
            f->size = f->pos;
            f->dirty = 1;
        }
    }

    rc = 0;

out:
    libos_heap_free(block);

    /* A partially completed write still moved the position and may have
     * grown the file, so report what landed rather than the error — the
     * caller can see the short count. A hard failure with nothing written
     * reports the error itself. */
    if (rc < 0 && done == 0) return rc;
    return (int64_t)done;
}

int64_t exofs_seek(exofs_file_t *f, int64_t off, uint32_t whence)
{
    if (f == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;

    int64_t base;
    switch (whence) {
    case EXOFS_SEEK_SET: base = 0;                break;
    case EXOFS_SEEK_CUR: base = (int64_t)f->pos;  break;
    case EXOFS_SEEK_END: base = (int64_t)f->size; break;
    default: return -EXO_EINVAL;
    }

    int64_t want = base + off;
    if (want < 0) return -EXO_EINVAL;
    if (want > (int64_t)0xFFFFFFFF) return -EXO_EINVAL;

    f->pos = (uint32_t)want;
    return want;
}

/* ---- stat / unlink ------------------------------------------------------ */

int exofs_stat(const char *path, exofs_stat_t *out)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    exofs_dirent_t e;
    int rc = exofs_path_resolve(v, path, NULL, &e);
    if (rc < 0) return rc;

    memset(out, 0, sizeof(*out));
    out->size       = e.size;
    out->attributes = e.attributes;
    out->ctime      = e.ctime;
    out->mtime      = e.mtime;
    return 0;
}

int exofs_unlink(const char *path)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;

    exofs_entry_ref_t ref;
    exofs_dirent_t e;

    int rc = exofs_path_resolve(v, path, &ref, &e);
    if (rc < 0) return rc;

    /* Directories go through rmdir, which checks emptiness. Quietly
     * removing a tree here is help nobody asked for. */
    if (e.attributes & EXOFS_ATTR_DIRECTORY) return -EXO_EISDIR;
    if (ref.block == EXOFS_NO_BLOCK)          return -EXO_EISDIR;

    /*
     * Unlink from the parent first, then free the data chain — the same
     * ordering as exofs_rmdir(), and for the same reason
     * (docs/filesystem.md §1). An interruption here leaks a chain nothing
     * references; the other order leaves a live directory entry naming
     * blocks that are already back on the free list.
     */
    rc = exofs_dir_remove(v, &ref);
    if (rc < 0) return rc;

    if (e.first_block != EXOFS_NO_BLOCK) {
        rc = exofs_chain_free(v, e.first_block);
        if (rc < 0) return rc;
    }

    return exofs_sync();
}
