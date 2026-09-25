/*
 * exofs_name.c — the variable-length name area (SCRUM-189).
 * See exofs_name.h for the design and for why cfat has no equivalent.
 *
 * Every function here works on one block at a time through a caller-owned
 * 512-byte buffer, because that is the unit a name record is guaranteed to
 * fit in. The volume's own v->scratch is deliberately NOT used: the callers
 * of this file (exofs_dirent.c, shortly) are in the middle of their own
 * read-modify-write on a directory block when they ask for a name, and
 * sharing one staging buffer between the two would have each quietly
 * overwrite the other.
 */

#include "exofs_name.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs_fat.h"
#include "exofs.h"

#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* ---- Record header ------------------------------------------------------
 *
 * Read and written byte-wise rather than through a uint16_t*: a record sits
 * at whatever offset the previous one ended at, which is only 2-byte aligned
 * when every capacity happens to be even. x86 tolerates the unaligned
 * access, but the format should not depend on that tolerance.
 */

static uint16_t hdr_read(const uint8_t *blk, uint16_t off)
{
    return (uint16_t)(blk[off] | ((uint16_t)blk[off + 1u] << 8));
}

static void hdr_write(uint8_t *blk, uint16_t off, uint16_t val)
{
    blk[off]      = (uint8_t)(val & 0xFFu);
    blk[off + 1u] = (uint8_t)(val >> 8);
}

static uint16_t hdr_cap(uint16_t h)  { return h & EXOFS_NAME_CAP_MASK; }
static int      hdr_free(uint16_t h) { return (h & EXOFS_NAME_FREE_BIT) != 0; }

/*
 * A header of 0 means "unused from here to the end of the block" rather than
 * a zero-capacity record. That falls out of exofs_fat_alloc() zeroing every
 * block it hands out, so a fresh name block needs no initialisation pass.
 */
static int hdr_is_unused_tail(uint16_t h) { return h == 0; }

/* The smallest record worth leaving behind when splitting: a header plus at
 * least one byte. Anything smaller cannot hold a name, so the remainder
 * stays with the record being allocated instead of becoming an unusable
 * hole the scan still has to step over. */
#define MIN_SPLIT_RECORD (EXOFS_NAME_HDR_SIZE + 1u)

/* ---- Block-level allocation ---------------------------------------------
 *
 * Find a home for `need` bytes in one block: a free record big enough, or
 * the unused tail. Returns the offset of the record's *data* through
 * off_out, having written the header, or -EXO_ENOSPC if this block cannot
 * take it. Mutates `blk` in memory only; the caller writes it back.
 */
static int place_in_block(uint8_t *blk, uint16_t need, uint16_t *off_out)
{
    uint16_t off = 0;

    while (off + EXOFS_NAME_HDR_SIZE <= EXOFS_BLOCK_SIZE) {
        uint16_t h = hdr_read(blk, off);

        if (hdr_is_unused_tail(h)) {
            uint32_t end = (uint32_t)off + EXOFS_NAME_HDR_SIZE + need;
            if (end > EXOFS_BLOCK_SIZE) return -EXO_ENOSPC;

            hdr_write(blk, off, need);

            /*
             * Zero the header the new tail starts at.
             *
             * It is tempting to skip this on the grounds that
             * exofs_fat_alloc() zeroes every block, so the bytes past the
             * tail are already zero. That is true only of a block nothing
             * has been freed in. coalesce_block() hands space back to the
             * tail by zeroing one header and leaving the record's old
             * *contents* in place — so after a free-then-allocate cycle the
             * bytes just past a bumped record are whatever name used to live
             * there, and the next scan reads them as a record with a
             * garbage capacity.
             *
             * Found by test_oversized_record_is_split, which is the shortest
             * sequence that reaches it: allocate, free (tail absorbs the
             * record but not its bytes), allocate something smaller, then
             * allocate again.
             */
            if (end + EXOFS_NAME_HDR_SIZE <= EXOFS_BLOCK_SIZE) {
                hdr_write(blk, (uint16_t)end, 0);
            }

            *off_out = (uint16_t)(off + EXOFS_NAME_HDR_SIZE);
            return 0;
        }

        uint16_t cap = hdr_cap(h);

        /* A capacity of 0 in a non-zero header, or one that runs past the
         * end of the block, means the block is corrupt. Stop rather than
         * loop forever or read past the end. */
        if (cap == 0) return -EXO_EIO;
        if ((uint32_t)off + EXOFS_NAME_HDR_SIZE + cap > EXOFS_BLOCK_SIZE) {
            return -EXO_EIO;
        }

        if (hdr_free(h) && cap >= need) {
            /* Split when the leftover can hold a record of its own;
             * otherwise the whole capacity goes to this name and the few
             * spare bytes ride along with it. */
            uint16_t leftover = (uint16_t)(cap - need);
            if (leftover >= MIN_SPLIT_RECORD) {
                uint16_t rest_hdr = (uint16_t)(off + EXOFS_NAME_HDR_SIZE + need);
                hdr_write(blk, off, need);
                hdr_write(blk, rest_hdr,
                          (uint16_t)((leftover - EXOFS_NAME_HDR_SIZE)
                                     | EXOFS_NAME_FREE_BIT));
            } else {
                hdr_write(blk, off, cap);   /* live, same footprint */
            }

            *off_out = (uint16_t)(off + EXOFS_NAME_HDR_SIZE);
            return 0;
        }

        off = (uint16_t)(off + EXOFS_NAME_HDR_SIZE + cap);
    }

    return -EXO_ENOSPC;
}

/*
 * Merge each run of adjacent free records in `blk` into one.
 *
 * Without this, alternating alloc/free of different sizes leaves a block
 * full of small free records that no later name fits in, while the unused
 * tail is exhausted — the classic fragmentation failure, and one this
 * filesystem would hit quickly under SCRUM-104's save rotation.
 *
 * A free record that runs to the unused tail is turned back into tail
 * (header zeroed) rather than left as a free record, so the tail can grow
 * back and the block returns to its pristine shape when everything in it is
 * freed. That is what lets the reuse tests assert the name chain does not
 * grow.
 */
static int coalesce_block(uint8_t *blk)
{
    uint16_t off = 0;

    while (off + EXOFS_NAME_HDR_SIZE <= EXOFS_BLOCK_SIZE) {
        uint16_t h = hdr_read(blk, off);
        if (hdr_is_unused_tail(h)) return 0;

        uint16_t cap = hdr_cap(h);
        if (cap == 0) return -EXO_EIO;
        if ((uint32_t)off + EXOFS_NAME_HDR_SIZE + cap > EXOFS_BLOCK_SIZE) {
            return -EXO_EIO;
        }

        if (!hdr_free(h)) {
            off = (uint16_t)(off + EXOFS_NAME_HDR_SIZE + cap);
            continue;
        }

        /* Absorb every following free record into this one. */
        uint16_t next = (uint16_t)(off + EXOFS_NAME_HDR_SIZE + cap);
        for (;;) {
            if (next + EXOFS_NAME_HDR_SIZE > EXOFS_BLOCK_SIZE) break;

            uint16_t nh = hdr_read(blk, next);

            if (hdr_is_unused_tail(nh)) {
                /* Adjacent to the tail: give the space back to the tail
                 * entirely. */
                hdr_write(blk, off, 0);
                return 0;
            }

            uint16_t ncap = hdr_cap(nh);
            if (ncap == 0) return -EXO_EIO;
            if ((uint32_t)next + EXOFS_NAME_HDR_SIZE + ncap > EXOFS_BLOCK_SIZE) {
                return -EXO_EIO;
            }
            if (!hdr_free(nh)) break;

            cap  = (uint16_t)(cap + EXOFS_NAME_HDR_SIZE + ncap);
            next = (uint16_t)(next + EXOFS_NAME_HDR_SIZE + ncap);
            hdr_write(blk, off, (uint16_t)(cap | EXOFS_NAME_FREE_BIT));
        }

        off = next;
    }

    return 0;
}

/* ---- Name chain --------------------------------------------------------- */

int exofs_name_chain_len(exofs_volume_t *v, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    if (v->name_head == EXOFS_NO_BLOCK) { *out = 0; return 0; }

    return exofs_chain_len(v, v->name_head, out);
}

/* ---- Allocation --------------------------------------------------------- */

int exofs_name_alloc(exofs_volume_t *v, const char *name, uint32_t len,
                     uint32_t *blk_out, uint16_t *off_out)
{
    if (v == NULL || name == NULL || blk_out == NULL || off_out == NULL) {
        return -EXO_EINVAL;
    }
    if (len == 0 || len > EXOFS_MAX_NAME) return -EXO_EINVAL;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    int rc;
    uint16_t need = (uint16_t)len;

    /*
     * First name on this volume: start the chain and record its head in the
     * superblock.
     *
     * FAT flush BEFORE the superblock write, and the ordering is the same
     * invariant as the two in docs/filesystem.md §1 — order writes so an
     * interruption leaves a state the filesystem already handles:
     *
     *   crash before the flush   the block is still free on disk and
     *                            name_head is still NO_BLOCK. Consistent.
     *   crash between the two    the block is allocated but referenced by
     *                            nothing. One leaked block; harmless, and
     *                            in particular not handed to a second
     *                            owner later.
     *   superblock first instead name_head would point at a block the
     *                            on-disk FAT calls free, so the next
     *                            allocation hands the name area's own
     *                            block to a file.
     */
    if (v->name_head == EXOFS_NO_BLOCK) {
        uint32_t first;
        rc = exofs_fat_alloc(v, &first);
        if (rc < 0) goto out;

        rc = exofs_sync();
        if (rc < 0) goto out;

        v->name_head = first;
        rc = exofs_super_update(v);
        if (rc < 0) { v->name_head = EXOFS_NO_BLOCK; goto out; }
    }

    /* Walk the chain looking for a block with room. */
    uint32_t blk = v->name_head;
    for (uint32_t steps = 0; steps <= v->total_blocks; steps++) {
        rc = exofs_read_block(blk, buf);
        if (rc < 0) goto out;

        uint16_t off;
        rc = place_in_block(buf, need, &off);
        if (rc == 0) {
            memcpy(buf + off, name, len);
            rc = exofs_write_block(blk, buf);
            if (rc < 0) goto out;

            *blk_out = blk;
            *off_out = off;
            goto out;
        }
        if (rc != -EXO_ENOSPC) goto out;   /* -EXO_EIO: corrupt block */

        uint32_t next;
        rc = exofs_fat_get(v, blk, &next);
        if (rc < 0) goto out;

        if (next == EXOFS_BLOCK_EOC) {
            /* Every existing block is full; grow the chain. The new block
             * is zeroed by exofs_fat_alloc(), so it is an empty name block
             * with no further setup. */
            uint32_t fresh;
            rc = exofs_chain_extend(v, v->name_head, &fresh);
            if (rc < 0) goto out;
            blk = fresh;
            continue;
        }
        if (next == EXOFS_BLOCK_FREE) { rc = -EXO_EIO; goto out; }

        blk = next;
    }

    rc = -EXO_EIO;   /* cycle in the name chain */

out:
    libos_heap_free(buf);
    return rc;
}

/* ---- Lookup and release -------------------------------------------------
 *
 * Both start by reading the block and validating that (off) really points at
 * a live record of at least the expected size. A dirent carrying a stale or
 * corrupt reference is exactly what these checks exist for: without them a
 * bad name_off would have memcpy reading from the middle of another record,
 * or past the end of the block.
 */
static int load_record(uint32_t blk, uint16_t off,
                       uint8_t *buf, uint16_t *cap_out, int *free_out)
{
    if (off < EXOFS_NAME_HDR_SIZE)     return -EXO_EINVAL;
    if (off > EXOFS_BLOCK_SIZE)        return -EXO_EINVAL;

    int rc = exofs_read_block(blk, buf);
    if (rc < 0) return rc;

    uint16_t h   = hdr_read(buf, (uint16_t)(off - EXOFS_NAME_HDR_SIZE));
    uint16_t cap = hdr_cap(h);

    if (hdr_is_unused_tail(h)) return -EXO_EINVAL;
    if (cap == 0)              return -EXO_EINVAL;
    if ((uint32_t)off + cap > EXOFS_BLOCK_SIZE) return -EXO_EINVAL;

    *cap_out  = cap;
    *free_out = hdr_free(h);
    return 0;
}

int exofs_name_read(exofs_volume_t *v, uint32_t blk, uint16_t off,
                    uint32_t len, char *out)
{
    if (v == NULL || out == NULL)          return -EXO_EINVAL;
    if (len == 0 || len > EXOFS_MAX_NAME)  return -EXO_EINVAL;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    uint16_t cap; int is_free;
    int rc = load_record(blk, off, buf, &cap, &is_free);
    if (rc < 0) goto out;

    if (is_free || len > cap) { rc = -EXO_EINVAL; goto out; }

    memcpy(out, buf + off, len);
    out[len] = '\0';
    rc = 0;

out:
    libos_heap_free(buf);
    return rc;
}

int exofs_name_equals(exofs_volume_t *v, uint32_t blk, uint16_t off,
                      uint32_t len, const char *cmp)
{
    if (v == NULL || cmp == NULL)          return -EXO_EINVAL;
    if (len == 0 || len > EXOFS_MAX_NAME)  return -EXO_EINVAL;

    /* A different length cannot be the same name, and answering that
     * without touching the disk is the point of keeping name_len in the
     * dirent — a directory scan rejects most candidates here. */
    if (strlen(cmp) != len) return 0;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    uint16_t cap; int is_free;
    int rc = load_record(blk, off, buf, &cap, &is_free);
    if (rc < 0) goto out;

    if (is_free || len > cap) { rc = -EXO_EINVAL; goto out; }

    rc = (memcmp(buf + off, cmp, len) == 0) ? 1 : 0;

out:
    libos_heap_free(buf);
    return rc;
}

int exofs_name_free(exofs_volume_t *v, uint32_t blk, uint16_t off)
{
    if (v == NULL) return -EXO_EINVAL;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    uint16_t cap; int is_free;
    int rc = load_record(blk, off, buf, &cap, &is_free);
    if (rc < 0) goto out;

    /* Freeing an already-free record means two dirents referenced it, or one
     * was released twice. Same reasoning as exofs_fat_free()'s double-free
     * check: worth hearing about rather than absorbing. */
    if (is_free) { rc = -EXO_EINVAL; goto out; }

    hdr_write(buf, (uint16_t)(off - EXOFS_NAME_HDR_SIZE),
              (uint16_t)(cap | EXOFS_NAME_FREE_BIT));

    rc = coalesce_block(buf);
    if (rc < 0) goto out;

    rc = exofs_write_block(blk, buf);

out:
    libos_heap_free(buf);
    return rc;
}
