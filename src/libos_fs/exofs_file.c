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
 *   allocated block are already zero. append_run()'s unzeroed blocks do
 *   not break this: it only runs at the chain's end and writes every byte
 *   of every block it claims (docs/filesystem.md §1.1).
 */

#include "exofs.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs_dir.h"
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

/* How many blocks exofs_read()'s whole-block path batches into one
 * exofs_bdev_read() call (SCRUM-190). Deliberately NOT read straight into
 * the caller's buffer: exofs_blockdev.h's own contract is that a disk I/O
 * buffer must lie in the LibOS VA window, and exofs_read()'s caller is not
 * guaranteed to -- doom_wad_disk.c, the reason this batching exists at all,
 * hands in kernel memory from alloc_pages_contig_owned() precisely because
 * it is NOT a LibOS-window buffer. So every transfer still bounces through
 * an in-window scratch buffer; what changes is that scratch buffer now
 * holds a batch of blocks instead of exactly one, so one exo_disk_read
 * syscall (and, below this file, one multi-sector ATA command) covers the
 * whole batch and one memcpy lands it in dst, rather than one syscall and
 * one memcpy per 512 bytes. 128 matches EXO_DISK_MAX_SECTORS
 * (src/syscall_disk.h) so a batch is exactly one syscall's worth rather
 * than being chunked again inside exofs_bdev_read(); this file does not
 * include that header; see exofs_blockdev.h. */
#define READ_BATCH_BLOCKS 128u

/* exofs_write()'s counterpart to READ_BATCH_BLOCKS (SCRUM-233): how many
 * whole blocks one append batches into a single exofs_bdev_write(), through
 * an in-window scratch buffer for the same reason reads bounce. Before
 * this, every appended block cost a zeroing write, a data write and a walk
 * of the whole chain to find its tail -- over three minutes to write the
 * 28 MB IWAD onto a volume from the shell's `install`, a few seconds now. */
#define WRITE_BATCH_BLOCKS 128u

/* ---- Chain navigation ---------------------------------------------------
 *
 * block_at() finds the block holding chain index `want`, using and updating
 * the handle's cached position. ensure_block() does the same but allocates
 * whatever is missing. contig_run() looks ahead from an already-found block
 * for exofs_read()'s bulk path below.
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

/* How many further chain blocks after `blk` (chain index `idx`) sit at
 * consecutive on-disk block numbers -- blk+1, blk+2, ... -- so exofs_read()
 * can hand them to exofs_bdev_read() as one transfer instead of one
 * exofs_read_block() per block (SCRUM-190: that was one exo_disk_read
 * syscall per 512-byte block, ~57,000 of them for a 28 MB file). Pure
 * v->fat[] lookups -- it is memory-resident (exofs_internal.h) -- so this
 * costs no disk I/O on top of the FAT-chain walk block_at() would have
 * made one hop at a time anyway. Bounded by `max` (how many whole blocks
 * the caller still wants) and stops at the first non-contiguous hop or the
 * chain's own end. *last_blk is the block the run ends on, so the caller
 * can fast-forward the handle's position cache past the whole run without
 * re-walking it. */
static uint32_t contig_run(exofs_volume_t *v, uint32_t blk, uint32_t max,
                           uint32_t *last_blk)
{
    uint32_t run = 1;
    uint32_t cur = blk;

    while (run < max) {
        uint32_t next;
        if (exofs_fat_get(v, cur, &next) < 0) break;
        if (next != cur + 1) break;
        cur = next;
        run++;
    }

    *last_blk = cur;
    return run;
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
    if (f == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;

    exofs_volume_t *v = f->vol;
    if (!exofs_vol_live(v, f->vol_gen)) return -EXO_EBADF;

    int rc = flush_metadata(v, f);
    if (rc < 0) return rc;

    return exofs_sync(v);
}

int exofs_close(exofs_file_t *f)
{
    if (f == NULL) return -EXO_EINVAL;
    if (!f->open) return 0;

    /* The volume may have been unmounted under the handle; exofs_fflush()
     * then reports -EXO_EBADF, and there is no open-handle table left to
     * clear. */
    int rc = exofs_fflush(f);
    f->open = 0;

    if (exofs_vol_live(f->vol, f->vol_gen)) {
        exofs_handle_unregister(f->vol, f->ent_block, f->ent_index);
    }

    return rc;
}

/* ---- Open --------------------------------------------------------------- */

int exofs_open(exofs_vol_t *v, const char *path, uint32_t flags,
               exofs_file_t *out)
{
    if (v == NULL || !v->mounted || out == NULL) return -EXO_EINVAL;

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

    /*
     * Refuse to truncate an entry another handle already has open. Without
     * this, O_TRUNC frees the chain out from under that other handle, which
     * keeps caching the now-freed first_block/cur_block and walks into
     * whatever the allocator hands out next — see
     * exofs_volume_t::open_ent_block.
     */
    if ((flags & EXOFS_O_TRUNC) && e.first_block != EXOFS_NO_BLOCK &&
        exofs_handle_is_open(v, ref.block, ref.index)) {
        return -EXO_EBUSY;
    }

    memset(out, 0, sizeof(*out));
    out->open        = 1;
    out->vol         = v;
    out->vol_gen     = v->gen;
    out->flags       = flags;
    out->ent_block   = ref.block;
    out->ent_index   = ref.index;
    out->first_block = e.first_block;
    out->size        = e.size;
    out->pos         = 0;
    out->dirty       = 0;
    out->cur_block   = EXOFS_NO_BLOCK;
    out->cur_index   = 0;

    rc = exofs_handle_register(v, out->ent_block, out->ent_index);
    if (rc < 0) { out->open = 0; return rc; }

    if ((flags & EXOFS_O_TRUNC) && out->first_block != EXOFS_NO_BLOCK) {
        rc = exofs_chain_free(v, out->first_block);
        if (rc < 0) goto fail_registered;

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
        if (rc < 0) goto fail_registered;

        rc = exofs_sync(v);
        if (rc < 0) goto fail_registered;
    }

    if (flags & EXOFS_O_APPEND) out->pos = out->size;

    return 0;

fail_registered:
    /* A truncate step failed after the handle was already registered — undo
     * that registration so a failed open does not leave a phantom entry in
     * the table, blocking a later truncate/unlink for no reason. */
    exofs_handle_unregister(v, out->ent_block, out->ent_index);
    out->open = 0;
    return rc;
}

/* ---- Read / write ------------------------------------------------------- */

int64_t exofs_read(exofs_file_t *f, void *buf, uint32_t n)
{
    if (f == NULL || buf == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;

    exofs_volume_t *v = f->vol;
    if (!exofs_vol_live(v, f->vol_gen)) return -EXO_EBADF;
    if (!(f->flags & EXOFS_O_READ)) return -EXO_EACCES;

    if (f->pos >= f->size || n == 0) return 0;

    /* Clamp to what is actually there: a short read at EOF, never a read of
     * whatever the last block happens to still hold past the size. */
    uint32_t avail = f->size - f->pos;
    if (n > avail) n = avail;

    /* A scratch buffer the disk I/O actually targets -- see
     * READ_BATCH_BLOCKS's own comment for why this cannot be dst itself.
     * Sized to what this call could actually use: at most READ_BATCH_BLOCKS,
     * but never more than n needs (the "+ 1" covers a leading/trailing
     * partial block), so a small read -- most of them, in practice -- costs
     * the same one-block scratch allocation it always did. */
    uint32_t want_blocks = n / EXOFS_BLOCK_SIZE + 1u;
    uint32_t batch_blocks = want_blocks < READ_BATCH_BLOCKS
                          ? want_blocks : READ_BATCH_BLOCKS;
    uint8_t *block = libos_heap_alloc(batch_blocks * EXOFS_BLOCK_SIZE);
    if (block == NULL) return -EXO_ENOMEM;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;
    int rc = 0;

    while (done < n) {
        uint32_t idx = f->pos / EXOFS_BLOCK_SIZE;
        uint32_t off = f->pos % EXOFS_BLOCK_SIZE;

        uint32_t blk;
        rc = block_at(v, f, idx, &blk);
        if (rc < 0) goto out;

        if (off == 0 && n - done >= EXOFS_BLOCK_SIZE) {
            /* A run of whole blocks: batch every block that is contiguous
             * on disk from here, up to what is left to read and
             * batch_blocks, into one exofs_bdev_read() into the scratch
             * buffer, then one bulk memcpy into dst. Still far fewer
             * syscalls than one exofs_read_block() per block (SCRUM-190:
             * ~57,000 of them for a 28 MB file down to ~450), just not
             * zero-copy -- see READ_BATCH_BLOCKS. */
            uint32_t whole_left = (n - done) / EXOFS_BLOCK_SIZE;
            uint32_t cap = whole_left < batch_blocks
                         ? whole_left : batch_blocks;
            uint32_t last;
            uint32_t run = contig_run(v, blk, cap, &last);

            rc = exofs_bdev_read(v->dev, exofs_block_lba(v, blk), block, run);
            if (rc < 0) goto out;
            v->stat_block_reads += run;

            memcpy(dst + done, block, (size_t)run * EXOFS_BLOCK_SIZE);

            f->cur_block = last;
            f->cur_index = idx + run - 1;

            uint32_t bytes = run * EXOFS_BLOCK_SIZE;
            done   += bytes;
            f->pos += bytes;
            continue;
        }

        /* The read's leading or trailing partial block. */
        uint32_t take = EXOFS_BLOCK_SIZE - off;
        if (take > n - done) take = n - done;

        rc = exofs_read_block(v, blk, block);
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

/*
 * exofs_write()'s bulk path: when chain index `idx` is exactly one past the
 * end of the file's chain (or the file has no chain and idx is 0), claim a
 * run of consecutive blocks with exofs_fat_alloc_run(), write `src` into it
 * with one exofs_bdev_write() through `scratch`, and only then link it onto
 * the chain. Returns the number of whole blocks written, 0 if this is not
 * such an append (the caller falls back to one block at a time, which
 * handles overwrites and holes), or a negative error with nothing changed.
 *
 * Writing before linking, and giving the run back on a failed write, is
 * what keeps exofs_fat_alloc_run()'s no-zeroing safe: see exofs_fat.h.
 */
static int append_run(exofs_volume_t *v, exofs_file_t *f, uint32_t idx,
                      const uint8_t *src, uint32_t len, uint8_t *scratch,
                      uint32_t batch_blocks)
{
    uint32_t tail = EXOFS_NO_BLOCK;
    if (f->first_block == EXOFS_NO_BLOCK) {
        if (idx != 0) return 0;
    } else {
        /* One walk to idx - 1, then one FAT lookup: idx is an append only
         * when idx - 1 is the chain's last block. */
        uint32_t next;
        if (idx == 0) return 0;
        int rc = block_at(v, f, idx - 1, &tail);
        if (rc == -EXO_EINVAL) return 0;    /* a hole before idx */
        if (rc < 0) return rc;
        rc = exofs_fat_get(v, tail, &next);
        if (rc < 0) return rc;
        if (next != EXOFS_BLOCK_EOC) return 0;  /* block exists: overwrite */
    }

    uint32_t whole = len / EXOFS_BLOCK_SIZE;
    uint32_t cap = whole < batch_blocks ? whole : batch_blocks;
    uint32_t first, run;
    int rc = exofs_fat_alloc_run(v, cap, &first, &run);
    if (rc < 0) return rc;

    memcpy(scratch, src, (size_t)run * EXOFS_BLOCK_SIZE);
    rc = exofs_bdev_write(v->dev, exofs_block_lba(v, first), scratch, run);
    if (rc < 0) {
        for (uint32_t i = 0; i < run; i++) {
            (void)exofs_fat_free(v, first + i);
        }
        return rc;
    }
    v->stat_block_writes += run;

    if (tail == EXOFS_NO_BLOCK) {
        f->first_block = first;
        f->dirty = 1;
    } else {
        rc = exofs_fat_set(v, tail, first);
        if (rc < 0) {
            for (uint32_t i = 0; i < run; i++) {
                (void)exofs_fat_free(v, first + i);
            }
            return rc;
        }
    }
    f->cur_block = first + run - 1u;
    f->cur_index = idx + run - 1u;
    return (int)run;
}

int64_t exofs_write(exofs_file_t *f, const void *buf, uint32_t n)
{
    if (f == NULL || buf == NULL) return -EXO_EINVAL;
    if (!f->open) return -EXO_EBADF;

    exofs_volume_t *v = f->vol;
    if (!exofs_vol_live(v, f->vol_gen)) return -EXO_EBADF;
    if (!(f->flags & EXOFS_O_WRITE)) return -EXO_EACCES;

    if (n == 0) return 0;

    if (f->flags & EXOFS_O_APPEND) f->pos = f->size;

    /* A file's size is a uint32_t on disk, so refuse rather than wrap. */
    if (n > 0xFFFFFFFFu - f->pos) return -EXO_EFBIG;

    /* Sized like exofs_read()'s scratch: a batch at most, one block for a
     * small write. */
    uint32_t want_blocks = n / EXOFS_BLOCK_SIZE + 1u;
    uint32_t batch_blocks = want_blocks < WRITE_BATCH_BLOCKS
                          ? want_blocks : WRITE_BATCH_BLOCKS;
    uint8_t *block = libos_heap_alloc(batch_blocks * EXOFS_BLOCK_SIZE);
    if (block == NULL) return -EXO_ENOMEM;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;
    int rc = 0;

    while (done < n) {
        uint32_t idx = f->pos / EXOFS_BLOCK_SIZE;
        uint32_t off = f->pos % EXOFS_BLOCK_SIZE;
        uint32_t put = EXOFS_BLOCK_SIZE - off;
        if (put > n - done) put = n - done;

        if (off == 0 && n - done >= EXOFS_BLOCK_SIZE) {
            rc = append_run(v, f, idx, src + done, n - done, block,
                            batch_blocks);
            if (rc < 0) goto out;
            if (rc > 0) {
                uint32_t bytes = (uint32_t)rc * EXOFS_BLOCK_SIZE;
                done   += bytes;
                f->pos += bytes;
                if (f->pos > f->size) {
                    f->size = f->pos;
                    f->dirty = 1;
                }
                continue;
            }
            /* 0: not an append at the chain's end; one block at a time. */
        }

        uint32_t blk;
        rc = ensure_block(v, f, idx, &blk);
        if (rc < 0) goto out;

        if (put == EXOFS_BLOCK_SIZE) {
            /* Whole block: no need to read what is about to be replaced. */
            memcpy(block, src + done, put);
        } else {
            rc = exofs_read_block(v, blk, block);
            if (rc < 0) goto out;
            memcpy(block + off, src + done, put);
        }

        rc = exofs_write_block(v, blk, block);
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
    if (!exofs_vol_live(f->vol, f->vol_gen)) return -EXO_EBADF;

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

int exofs_stat(exofs_vol_t *v, const char *path, exofs_stat_t *out)
{
    if (v == NULL || !v->mounted || out == NULL) return -EXO_EINVAL;

    exofs_dirent_t e;
    int rc = exofs_path_resolve(v, path, NULL, &e);
    if (rc < 0) return rc;

    /*
     * "." and ".." are real, independently-created dirents (exofs_dir.h),
     * not aliases resolved to their target's own entry — so their own
     * ctime/attributes/size are a second, separately-timestamped copy of
     * (approximately) the target's, not the target's real record. Reporting
     * them here would be silently wrong the moment the two ever drift, so
     * this is the same call exofs_rmdir() makes on the identical case rather
     * than a best-effort guess at which copy is "right".
     */
    if (exofs_dir_is_dot(v, &e)) return -EXO_EINVAL;

    memset(out, 0, sizeof(*out));
    out->size       = e.size;
    out->attributes = e.attributes;
    out->ctime      = e.ctime;
    out->mtime      = e.mtime;
    return 0;
}

int exofs_unlink(exofs_vol_t *v, const char *path)
{
    if (v == NULL || !v->mounted) return -EXO_EINVAL;

    exofs_entry_ref_t ref;
    exofs_dirent_t e;

    int rc = exofs_path_resolve(v, path, &ref, &e);
    if (rc < 0) return rc;

    /* Directories go through rmdir, which checks emptiness. Quietly
     * removing a tree here is help nobody asked for. */
    if (e.attributes & EXOFS_ATTR_DIRECTORY) return -EXO_EISDIR;
    if (ref.block == EXOFS_NO_BLOCK)          return -EXO_EISDIR;

    /* Refuse rather than free a chain a still-open handle caches — see
     * exofs_volume_t::open_ent_block and the same guard in exofs_open()'s
     * O_TRUNC path. */
    if (exofs_handle_is_open(v, ref.block, ref.index)) return -EXO_EBUSY;

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

    return exofs_sync(v);
}
