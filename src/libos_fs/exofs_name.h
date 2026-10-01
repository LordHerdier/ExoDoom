#ifndef EXOFS_NAME_H
#define EXOFS_NAME_H

#include <stdint.h>

#include "exofs_internal.h"

/*
 * exofs_name — the variable-length name area (SCRUM-189).
 *
 * THIS HAS NO cfat EQUIVALENT. cfat stored `char name[MAXFILENAME]` inline
 * in its directory entry, with MAXFILENAME = 11 — the 8.3 limit, and the
 * root of two of the bugs in its own README ("attempting to edit a mounted
 * file whose name is 11 characters long will result in a segfault", because
 * an 11-byte name in an 11-byte array leaves no room for the NUL that
 * strcpy() writes anyway). Removing that cap is what this ticket is for.
 *
 * WHY AN INDIRECTION AND NOT A LONGER ARRAY. A directory entry has to stay
 * fixed-size: at 32 bytes there are exactly 16 per block and an entry's
 * position is arithmetic rather than a scan, which is what the whole of
 * exofs_dirent.c is built on. A variable-length entry would make every
 * directory a linked list of records you can only find by reading all the
 * ones before it. So the entry stores a *reference* — (name_block, name_off,
 * name_len) — and the bytes live here.
 *
 * WHERE THE BYTES LIVE. In ordinary FAT-allocated data blocks, chained
 * through the FAT from the superblock's name_head. Nothing is reserved up
 * front: a volume with few names pays for few blocks. Records never straddle
 * a block boundary, so reading a name is always one block read — which caps
 * a name at EXOFS_BLOCK_SIZE - 2 by construction, well above the
 * EXOFS_MAX_NAME (255, POSIX NAME_MAX) this API advertises.
 *
 * THE HEADER HOLDS CAPACITY, NOT LENGTH. A record is a 2-byte header (low 15
 * bits capacity, high bit free) followed by `capacity` bytes. The name's
 * actual length is in the referring dirent's name_len, where it already had
 * to be. Storing capacity here is what lets a scan skip a record without
 * caring whether it is live, and lets a shorter name reuse a free record
 * without its footprint changing under the scan.
 *
 * REUSE, NOT COMPACTION. Freeing marks the record and coalesces it with
 * adjacent free records; allocation first-fits over free records before
 * bumping into unused space, and splits an oversized one when the leftover
 * can hold a record of its own. Compaction would mean rewriting every dirent
 * that points into the block, which requires an index this filesystem does
 * not have. Reuse matters concretely for SCRUM-104: Doom's save-file
 * rotation renames constantly, and a bump-only name area grows forever.
 */

/*
 * Store `name` (of `len` bytes, no NUL required) in the name area.
 *
 * Returns 0, with *blk_out and *off_out set to the reference a dirent
 * should record; -EXO_EINVAL for a zero-length name or one over
 * EXOFS_MAX_NAME; -EXO_ENOSPC if the volume is full; or a device error.
 *
 * May allocate a block and, the first time any name is stored on a volume,
 * rewrite the superblock to record the head of the name chain.
 */
int exofs_name_alloc(exofs_volume_t *v, const char *name, uint32_t len,
                     uint32_t *blk_out, uint16_t *off_out);

/*
 * Release the record at (blk, off).
 *
 * Does not free the containing block even when it becomes entirely unused.
 * Returning it to the FAT would mean unlinking it from the name chain, which
 * needs the previous block — findable, but the block is about to be reused
 * by the next name anyway, and an empty name block costs one block. Worth
 * revisiting only if name churn is ever shown to strand blocks in practice.
 *
 * Returns 0, -EXO_EINVAL for a reference that is out of range or does not
 * point at a live record, or a device error.
 */
int exofs_name_free(exofs_volume_t *v, uint32_t blk, uint16_t off);

/*
 * Copy the `len` bytes at (blk, off) into `out` and NUL-terminate, so `out`
 * must hold len + 1 bytes. `len` comes from the dirent.
 *
 * Returns 0, -EXO_EINVAL if the reference or length does not fit the record
 * found there, or a device error.
 */
int exofs_name_read(exofs_volume_t *v, uint32_t blk, uint16_t off,
                    uint32_t len, char *out);

/*
 * Whether the record at (blk, off) holds exactly `cmp` (a NUL-terminated
 * string), given the `len` from the dirent.
 *
 * Exists so directory lookup — the hot path, run once per entry per path
 * component — can answer without copying each candidate name out first.
 * Returns 1 for equal, 0 for not, or a negative error.
 */
int exofs_name_equals(exofs_volume_t *v, uint32_t blk, uint16_t off,
                      uint32_t len, const char *cmp);

/* Number of blocks in the name chain. For reporting, and for the tests that
 * assert reuse actually reuses rather than growing the chain. 0 when no name
 * has been stored yet. */
int exofs_name_chain_len(exofs_volume_t *v, uint32_t *out);

#endif /* EXOFS_NAME_H */
