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

/* ---- Room table (SCRUM-223) ----------------------------------------------
 *
 * exofs_volume_t::name_room, and exofs_internal.h says what it is and why it
 * is a table rather than a single hint. What matters here is the one rule
 * that makes it safe to consult:
 *
 *   an entry is written only from a block this file has just scanned from
 *   end to end, and only in the two places a name block ever changes.
 *
 * So an entry is always an exact statement about the block as this code last
 * left it, and place_in_block(need) would succeed on that block exactly when
 * need <= entry. Skipping a block whose entry is below `need` therefore
 * skips precisely the reads that would have ended in -EXO_ENOSPC, and the
 * allocation lands where it always did -- tests/kernel/test_exofs_k.c runs
 * the same workload with the table wiped before every call and compares the
 * placements record for record.
 *
 * The one thing that does change: a block already known to be too small is
 * no longer re-read, so damage that appears in it later (not through this
 * code) goes unnoticed until something needs that block again.
 */

/*
 * The longest name place_in_block() could put in `blk` right now: the
 * largest free record, or the unused tail if that is bigger. Mirrors that
 * function's own walk, so the two cannot disagree about what fits.
 *
 * EXOFS_NAME_ROOM_UNKNOWN for a block with a bad header. Nothing can be said
 * about such a block except that the next allocation should read it and
 * report it, exactly as before.
 */
static uint16_t block_room(const uint8_t *blk)
{
    uint16_t best = 0;
    uint16_t off = 0;

    while (off + EXOFS_NAME_HDR_SIZE <= EXOFS_BLOCK_SIZE) {
        uint16_t h = hdr_read(blk, off);

        if (hdr_is_unused_tail(h)) {
            uint16_t tail =
                (uint16_t)(EXOFS_BLOCK_SIZE - off - EXOFS_NAME_HDR_SIZE);
            return tail > best ? tail : best;
        }

        uint16_t cap = hdr_cap(h);
        if (cap == 0) return EXOFS_NAME_ROOM_UNKNOWN;
        if ((uint32_t)off + EXOFS_NAME_HDR_SIZE + cap > EXOFS_BLOCK_SIZE) {
            return EXOFS_NAME_ROOM_UNKNOWN;
        }

        if (hdr_free(h) && cap > best) best = cap;

        off = (uint16_t)(off + EXOFS_NAME_HDR_SIZE + cap);
    }

    return best;
}

/* Whether the table already knows the block at chain position `pos` cannot
 * take `need` bytes. "Don't know" is always answered no. */
static int room_rules_out(const exofs_volume_t *v, uint32_t pos, uint16_t need)
{
    if (v->name_room == NULL || pos >= v->name_room_cap) return 0;

    uint16_t room = v->name_room[pos];
    return room != EXOFS_NAME_ROOM_UNKNOWN && room < need;
}

/* Record what `blk`, the block at chain position `pos`, can take now. A
 * position past the table is dropped: it stays "read it and see". */
static void room_note(exofs_volume_t *v, uint32_t pos, const uint8_t *blk)
{
    if (v->name_room == NULL || pos >= v->name_room_cap) return;

    v->name_room[pos] = block_room(blk);
}

/* Small enough to cost nothing on a volume with a handful of names, and
 * doubled from there, so growing it stays amortised O(1) per name block. */
#define ROOM_TABLE_MIN 16u

/*
 * Make sure the table has an entry for every block of the chain, plus one
 * for the block this allocation may be about to add.
 *
 * Best effort. If the heap has nothing to give, the table stays as it is --
 * possibly NULL -- and the allocation carries on reading blocks it has no
 * entry for. That is slower and otherwise identical, which is the right
 * trade for a failure that is about a few bytes of bookkeeping: refusing to
 * store a name over it would turn an optimisation into a point of failure.
 */
static void room_reserve(exofs_volume_t *v)
{
    uint32_t len;
    if (exofs_name_chain_len(v, &len) < 0) return;

    uint32_t want = len + 1u;
    if (v->name_room != NULL && v->name_room_cap >= want) return;

    uint32_t cap = v->name_room_cap != 0 ? v->name_room_cap : ROOM_TABLE_MIN;
    while (cap < want) cap *= 2u;

    uint16_t *table = libos_heap_alloc((size_t)cap * sizeof(uint16_t));
    if (table == NULL) return;

    uint32_t kept = v->name_room != NULL ? v->name_room_cap : 0u;
    for (uint32_t i = 0; i < kept; i++) table[i] = v->name_room[i];
    for (uint32_t i = kept; i < cap; i++) table[i] = EXOFS_NAME_ROOM_UNKNOWN;

    libos_heap_free(v->name_room);
    v->name_room     = table;
    v->name_room_cap = cap;
}

/*
 * Where `blk` sits in the name chain (0 = name_head). Returns 1 with
 * *pos_out set, or 0 if it is not on the chain at all -- which a dirent
 * carrying a corrupt name reference can cause, and which must not be turned
 * into a table write at some unrelated position.
 *
 * An in-RAM FAT walk: no disk access.
 */
static int name_chain_pos(exofs_volume_t *v, uint32_t blk, uint32_t *pos_out)
{
    uint32_t cur = v->name_head;

    for (uint32_t pos = 0; pos <= v->total_blocks; pos++) {
        if (cur == blk) { *pos_out = pos; return 1; }

        uint32_t next;
        if (exofs_fat_get(v, cur, &next) < 0) return 0;
        if (next == EXOFS_BLOCK_EOC || next == EXOFS_BLOCK_FREE) return 0;

        cur = next;
    }

    return 0;
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

    room_reserve(v);

    /*
     * Walk the chain looking for a block with room -- first fit, in chain
     * order, as it has always been. `pos` counts blocks so the room table
     * can be consulted and kept up to date; the walk itself is over the
     * in-RAM FAT and costs no I/O.
     *
     * What used to make this quadratic was the read: every block on the way
     * to the one with room was fetched from disk just to be told it was
     * full, on every call (SCRUM-223). A block the table rules out is now
     * stepped over without being read, so the common case is one read, of
     * the block the name goes into.
     */
    uint32_t blk = v->name_head;
    for (uint32_t pos = 0; pos <= v->total_blocks; pos++) {
        if (!room_rules_out(v, pos, need)) {
            rc = exofs_read_block(blk, buf);
            if (rc < 0) goto out;

            uint16_t off;
            rc = place_in_block(buf, need, &off);
            if (rc == 0) {
                memcpy(buf + off, name, len);
                rc = exofs_write_block(blk, buf);
                if (rc < 0) goto out;   /* disk unchanged, so is the table */

                room_note(v, pos, buf);
                *blk_out = blk;
                *off_out = off;
                goto out;
            }
            if (rc != -EXO_ENOSPC) goto out;   /* -EXO_EIO: corrupt block */

            /* Scanned to the end and nothing fit: worth remembering, so the
             * next name this size or longer does not come back to ask. */
            room_note(v, pos, buf);
        }

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
    if (rc < 0) goto out;

    /* The block has room it did not have a moment ago, and the room table
     * has to hear about it: an entry left saying "full" would have every
     * later allocation step over this block without reading it, and the
     * record just freed would never be found again (SCRUM-223). After the
     * write, so the table never describes a block the disk does not hold. */
    uint32_t pos;
    if (name_chain_pos(v, blk, &pos)) room_note(v, pos, buf);

out:
    libos_heap_free(buf);
    return rc;
}
