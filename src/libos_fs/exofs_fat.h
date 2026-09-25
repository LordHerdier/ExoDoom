#ifndef EXOFS_FAT_H
#define EXOFS_FAT_H

#include <stdint.h>

#include "exofs_internal.h"

/*
 * exofs_fat — block allocation and chain traversal (SCRUM-189).
 *
 * The cfat equivalent is fat.c (findFreeBlock, allocateNewBlock,
 * findLastBlockOfParent). The chain representation is unchanged — a block's
 * FAT entry names the next block, or EXOFS_BLOCK_EOC to end — but four
 * things about how it is walked and allocated are not cfat's, and each one
 * is a bug that file has:
 *
 *   1. NO exit(). cfat's findFreeBlock() called exit(1) when the disk was
 *      full, taking the process with it. A full disk is -EXO_ENOSPC.
 *
 *   2. CYCLE SAFETY. cfat's chain walks are `while (FAT[i] != USHRT_MAX)`
 *      with no bound, so a FAT with a loop in it — which is what a torn
 *      write or a corrupted sector produces — hangs forever. Every walk
 *      here is bounded by the volume's block count and reports -EXO_EIO
 *      instead. A filesystem that reads from a real disk has to survive
 *      garbage on it; cfat only ever read a file it had written itself.
 *
 *   3. AN ALLOCATION HINT. cfat rescanned the FAT from block 0 on every
 *      allocation, which is O(n) per block and O(n²) to fill a volume — on
 *      a 128 MiB volume that is 262144 entries rescanned per block. The
 *      search here resumes from where the last one stopped and wraps once.
 *      Worst case is unchanged; the common case of allocating a run of
 *      blocks stops being quadratic.
 *
 *   4. BLOCK 0 IS NEVER ALLOCATED. It is the root directory. cfat relied on
 *      FAT[0] never reading as free to keep findFreeBlock() off it, which
 *      holds only as long as nothing ever frees it by mistake. The search
 *      here starts at block 1, so the root cannot be handed out even if the
 *      FAT on disk says it is free.
 *
 * WRITEBACK. These functions mark changed FAT sectors dirty; they do not
 * flush. The flush belongs to the operation, not the block: extending a file
 * by 100 blocks should cost one FAT writeback, not 100. Callers call
 * exofs_sync() at the point the operation is complete. Data blocks are a
 * different matter — see exofs_fat_alloc() below.
 */

/* Read/write one FAT entry. `blk` is bounds-checked against the volume, so
 * a chain walk that follows a corrupted link gets -EXO_EINVAL rather than
 * reading past the array. The FAT is in RAM, so this costs nothing worth
 * avoiding with an unchecked variant. */
int exofs_fat_get(exofs_volume_t *v, uint32_t blk, uint32_t *out);
int exofs_fat_set(exofs_volume_t *v, uint32_t blk, uint32_t val);

/*
 * Allocate one free block, mark it end-of-chain, and zero it on disk.
 *
 * The zeroing is not optional and not the caller's job. A newly allocated
 * block still holds whatever the last file to own it left there, and for a
 * directory block those bytes would be read as entries — a stale name with
 * a stale first_block, pointing into a chain that now belongs to somebody
 * else. Zeroing costs one 512-byte write that nearly every caller would
 * have had to do anyway.
 *
 * Returns 0 with *out set, -EXO_ENOSPC if the volume is full, -EXO_EINVAL
 * if nothing is mounted, or a device error from the zeroing write (in which
 * case nothing is allocated).
 */
int exofs_fat_alloc(exofs_volume_t *v, uint32_t *out);

/* Free a single block. Freeing an already-free block is -EXO_EINVAL rather
 * than a silent no-op: it means a chain was walked twice or a block was
 * double-owned, and both are worth hearing about. */
int exofs_fat_free(exofs_volume_t *v, uint32_t blk);

/*
 * Chain operations. `head` is the first block of a chain; all of these
 * detect a cycle and return -EXO_EIO rather than looping.
 */

/* Last block of the chain starting at `head`. */
int exofs_chain_last(exofs_volume_t *v, uint32_t head, uint32_t *out);

/* Number of blocks in the chain starting at `head`. */
int exofs_chain_len(exofs_volume_t *v, uint32_t head, uint32_t *out);

/* The `n`th block of the chain (0 = head). -EXO_EINVAL if the chain is
 * shorter than that — the caller asked for a block that does not exist,
 * which for a file read means an offset past the end. */
int exofs_chain_nth(exofs_volume_t *v, uint32_t head, uint32_t n,
                    uint32_t *out);

/*
 * Append one freshly allocated block to the end of the chain starting at
 * `head`, and return it. The new block is zeroed, per exofs_fat_alloc().
 *
 * On a full volume this returns -EXO_ENOSPC having changed nothing — the
 * allocation happens before the link is written, so a failure cannot leave
 * a chain pointing at a block that was never allocated.
 */
int exofs_chain_extend(exofs_volume_t *v, uint32_t head, uint32_t *out);

/*
 * Free every block of the chain starting at `head`.
 *
 * Walks the chain twice: once to validate it end to end, once to free it.
 * That way a cycle is found while the FAT still describes the chain, rather
 * than halfway through dismantling it — on -EXO_EIO nothing is freed, which
 * leaves the corruption intact for a repair tool to look at instead of
 * turning it into a half-freed chain. Two O(n) passes over an in-RAM array,
 * and no scratch storage, which a single pass recording blocks would need.
 */
int exofs_chain_free(exofs_volume_t *v, uint32_t head);

/* Number of free blocks on the volume, for reporting and for tests that
 * assert an operation gave back exactly what it took. O(total_blocks) over
 * the in-RAM FAT; not for a hot path. */
uint32_t exofs_fat_free_count(exofs_volume_t *v);

#endif /* EXOFS_FAT_H */
