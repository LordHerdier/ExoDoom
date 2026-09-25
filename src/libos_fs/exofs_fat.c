/*
 * exofs_fat.c — block allocation and chain traversal (SCRUM-189).
 * See exofs_fat.h for the design and for the four things this does
 * differently from cfat's fat.c.
 */

#include "exofs_fat.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs.h"

#include "exo_errno.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* Block 0 is the root directory and is never a candidate for allocation.
 * See exofs_fat.h point 4. */
#define FIRST_ALLOCATABLE_BLOCK 1u

/* ---- Entry access ------------------------------------------------------- */

int exofs_fat_get(exofs_volume_t *v, uint32_t blk, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;
    if (blk >= v->total_blocks)   return -EXO_EINVAL;

    *out = v->fat[blk];
    return 0;
}

int exofs_fat_set(exofs_volume_t *v, uint32_t blk, uint32_t val)
{
    if (v == NULL)              return -EXO_EINVAL;
    if (blk >= v->total_blocks) return -EXO_EINVAL;

    v->fat[blk] = val;
    exofs_fat_mark_dirty(v, blk);
    return 0;
}

/* ---- Allocation ---------------------------------------------------------
 *
 * One pass from the hint to the end, then one from the first allocatable
 * block back to the hint. Split in two rather than written as a single
 * modular loop because the wrap is the only interesting case and this way
 * it is visible rather than hidden in an index expression.
 */
static int find_free_block(exofs_volume_t *v, uint32_t *out)
{
    uint32_t start = v->next_free_hint;
    if (start < FIRST_ALLOCATABLE_BLOCK || start >= v->total_blocks) {
        start = FIRST_ALLOCATABLE_BLOCK;
    }

    for (uint32_t i = start; i < v->total_blocks; i++) {
        if (v->fat[i] == EXOFS_BLOCK_FREE) { *out = i; return 0; }
    }
    for (uint32_t i = FIRST_ALLOCATABLE_BLOCK; i < start; i++) {
        if (v->fat[i] == EXOFS_BLOCK_FREE) { *out = i; return 0; }
    }

    return -EXO_ENOSPC;
}

int exofs_fat_alloc(exofs_volume_t *v, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    uint32_t blk;
    int rc = find_free_block(v, &blk);
    if (rc < 0) return rc;

    /*
     * Zero the block on disk BEFORE claiming it in the FAT.
     *
     * The ordering matters for what an interrupted allocation leaves
     * behind. Zero-then-claim leaves, at worst, a zeroed block that is
     * still marked free — which is exactly a free block, so nothing is
     * lost. Claim-then-zero would leave a block owned by a chain and still
     * holding its previous owner's bytes, which for a directory block reads
     * as live entries pointing into somebody else's chain.
     *
     * exofs_fat.h's own comment covers why zeroing is not left to the
     * caller at all.
     */
    if (v->scratch == NULL) return -EXO_EINVAL;
    memset(v->scratch, 0, EXOFS_BLOCK_SIZE);

    rc = exofs_write_block(blk, v->scratch);
    if (rc < 0) return rc;

    v->fat[blk] = EXOFS_BLOCK_EOC;
    exofs_fat_mark_dirty(v, blk);

    /* Next search starts after this one. A run of allocations then walks
     * forward instead of rescanning from the beginning each time. */
    v->next_free_hint = blk + 1u;

    *out = blk;
    return 0;
}

int exofs_fat_free(exofs_volume_t *v, uint32_t blk)
{
    if (v == NULL)              return -EXO_EINVAL;
    if (blk >= v->total_blocks) return -EXO_EINVAL;

    /* Refusing a double free rather than ignoring it: see exofs_fat.h. */
    if (v->fat[blk] == EXOFS_BLOCK_FREE) return -EXO_EINVAL;

    v->fat[blk] = EXOFS_BLOCK_FREE;
    exofs_fat_mark_dirty(v, blk);

    /* Reuse it sooner rather than later — a freed block is the best
     * candidate for the next allocation and costs no scan to find. */
    if (blk >= FIRST_ALLOCATABLE_BLOCK) v->next_free_hint = blk;

    return 0;
}

/* ---- Chain traversal ----------------------------------------------------
 *
 * Every walk below is bounded by v->total_blocks. A chain cannot legally be
 * longer than the number of blocks on the volume, so exceeding that means
 * the FAT contains a cycle — which is what a torn write produces, and what
 * cfat's unbounded `while (FAT[i] != USHRT_MAX)` would spin on forever.
 * -EXO_EIO rather than -EXO_EINVAL: the argument was fine, the disk is not.
 */

int exofs_chain_last(exofs_volume_t *v, uint32_t head, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    uint32_t cur = head;
    for (uint32_t steps = 0; steps <= v->total_blocks; steps++) {
        uint32_t next;
        int rc = exofs_fat_get(v, cur, &next);
        if (rc < 0) return rc;

        if (next == EXOFS_BLOCK_EOC) { *out = cur; return 0; }

        /* A free entry inside a chain is corruption too: the chain claims a
         * block that the allocator believes nobody owns. */
        if (next == EXOFS_BLOCK_FREE) return -EXO_EIO;

        cur = next;
    }

    return -EXO_EIO;
}

int exofs_chain_len(exofs_volume_t *v, uint32_t head, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    uint32_t cur = head;
    for (uint32_t steps = 0; steps <= v->total_blocks; steps++) {
        uint32_t next;
        int rc = exofs_fat_get(v, cur, &next);
        if (rc < 0) return rc;

        if (next == EXOFS_BLOCK_EOC) { *out = steps + 1u; return 0; }
        if (next == EXOFS_BLOCK_FREE) return -EXO_EIO;

        cur = next;
    }

    return -EXO_EIO;
}

int exofs_chain_nth(exofs_volume_t *v, uint32_t head, uint32_t n,
                    uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;
    if (n > v->total_blocks)      return -EXO_EINVAL;

    uint32_t cur = head;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next;
        int rc = exofs_fat_get(v, cur, &next);
        if (rc < 0) return rc;

        /* Ran off the end: the caller asked for a block beyond the chain,
         * which for a file read is an offset past EOF. -EXO_EINVAL, not
         * -EXO_EIO — nothing is wrong with the disk. */
        if (next == EXOFS_BLOCK_EOC)  return -EXO_EINVAL;
        if (next == EXOFS_BLOCK_FREE) return -EXO_EIO;

        cur = next;
    }

    /* Confirm the block we landed on is itself part of a chain rather than
     * a stale index: reading its entry validates the bound. */
    uint32_t unused;
    int rc = exofs_fat_get(v, cur, &unused);
    if (rc < 0) return rc;

    *out = cur;
    return 0;
}

int exofs_chain_extend(exofs_volume_t *v, uint32_t head, uint32_t *out)
{
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    uint32_t last;
    int rc = exofs_chain_last(v, head, &last);
    if (rc < 0) return rc;

    /* Allocate first, link second. A failed allocation must not leave the
     * chain pointing at a block that was never claimed. */
    uint32_t blk;
    rc = exofs_fat_alloc(v, &blk);
    if (rc < 0) return rc;

    rc = exofs_fat_set(v, last, blk);
    if (rc < 0) {
        /* Undo the allocation rather than stranding it. The only way this
         * fails is a bad `last`, which exofs_chain_last() just validated,
         * so this is belt-and-braces — but a leaked block is invisible
         * until the volume fills up, which is the worst time to find out. */
        (void)exofs_fat_free(v, blk);
        return rc;
    }

    *out = blk;
    return 0;
}

int exofs_chain_free(exofs_volume_t *v, uint32_t head)
{
    if (v == NULL) return -EXO_EINVAL;

    /* Pass 1: validate the whole chain without touching it. See
     * exofs_fat.h on why this is not merged into the freeing pass. */
    uint32_t len;
    int rc = exofs_chain_len(v, head, &len);
    if (rc < 0) return rc;

    /* Pass 2: free it. The walk has to read each entry before clearing it,
     * so `next` is read first and the entry cleared after. */
    uint32_t cur = head;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next;
        rc = exofs_fat_get(v, cur, &next);
        if (rc < 0) return rc;

        rc = exofs_fat_free(v, cur);
        if (rc < 0) return rc;

        if (next == EXOFS_BLOCK_EOC) break;
        cur = next;
    }

    return 0;
}

uint32_t exofs_fat_free_count(exofs_volume_t *v)
{
    if (v == NULL) return 0;

    uint32_t n = 0;
    for (uint32_t i = FIRST_ALLOCATABLE_BLOCK; i < v->total_blocks; i++) {
        if (v->fat[i] == EXOFS_BLOCK_FREE) n++;
    }
    return n;
}
