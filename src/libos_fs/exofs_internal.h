#ifndef EXOFS_INTERNAL_H
#define EXOFS_INTERNAL_H

#include <stdint.h>

#include "exofs.h"
#include "exofs_layout.h"

/*
 * exofs_internal.h — shared state between ExoFS's own translation units
 * (SCRUM-189). Not a public header, and deliberately not exofs_layout.h:
 * this describes what a *mounted* volume looks like in memory, which is
 * ExoDoom-specific and of no use to SCRUM-190's image builder.
 *
 * THE FAT LIVES IN RAM. The whole FAT is read into exofs_volume_t::fat at
 * mount and written back a sector at a time as it changes. That is the
 * cache policy for this filesystem, entire: chain walks are the hot path and
 * a FAT lookup that cost a syscall would make every one of them a series of
 * interrupt-disabled disk transfers. Data and directory blocks are read
 * through on every access with no cache, so there is no dirty-block
 * invariant anywhere except the FAT's, and that one is a single bitmap.
 *
 * WHY A DIRTY BITMAP AND NOT A DIRTY FLAG. A 128 MiB volume has a 1 MiB
 * FAT. Writing all of it back because one entry changed would turn a
 * single-block allocation into a 2048-sector transfer, with interrupts off
 * the whole way (`syscall`'s FMASK clears IF). One bit per FAT *sector*
 * keeps a typical allocation to a single 512-byte write.
 */

typedef struct exofs_volume {
    int      mounted;

    /* Geometry, as recorded in and validated against the superblock. */
    uint32_t base_lba;       /* LBA of the superblock                     */
    uint32_t fat_lba;        /* base_lba + 1                              */
    uint32_t fat_blocks;     /* sectors the FAT occupies                  */
    uint32_t data_lba;       /* absolute LBA of data block 0              */
    uint32_t total_blocks;   /* data blocks the FAT covers                */
    uint32_t root_block;     /* EXOFS_ROOT_BLOCK                          */

    /*
     * The in-RAM FAT. Allocated fat_blocks * EXOFS_BLOCK_SIZE bytes, NOT
     * total_blocks * sizeof(exofs_fat_t) — the last FAT sector is usually
     * only partly used, and reading a whole number of sectors into a buffer
     * sized to the entry count would run past its end. Entries at index >=
     * total_blocks exist in this buffer and are never valid block numbers.
     *
     * From libos_heap_alloc(), never malloc(): it has to lie in the LibOS VA
     * window to be a legal exo_disk_read target. exofs_blockdev.h has the
     * full reasoning.
     */
    exofs_fat_t *fat;

    /* One bit per FAT sector, set when that sector differs from disk. */
    uint8_t  *fat_dirty;
    uint32_t  fat_dirty_bytes;

    /*
     * A single block-sized staging buffer in the LibOS window, for the
     * read-modify-write that every directory-entry update is. Owned by the
     * volume so each layer above does not allocate its own; callers that
     * need two blocks live at once (copying between blocks) must allocate
     * the second themselves.
     */
    uint8_t  *scratch;

    /*
     * Where the next free-block search starts. cfat rescanned the FAT from
     * block 0 on every allocation, making a run of N blocks O(N * total)
     * — on a 128 MiB volume, 262144 entries rescanned per block. The search
     * resumes here and wraps once, so allocating a run costs one pass in
     * total rather than one pass per block. Purely an optimisation: it is
     * never trusted, since a stale hint only costs a longer scan.
     */
    uint32_t next_free_hint;
} exofs_volume_t;

/* The one mounted volume, or NULL when nothing is mounted. */
exofs_volume_t *exofs_vol(void);

/* Absolute LBA of data block `blk`. Valid only for blk < total_blocks; the
 * caller checks, because every caller already has a reason to know whether
 * the index came from a FAT chain (trusted) or from a caller (not). */
uint32_t exofs_block_lba(const exofs_volume_t *v, uint32_t blk);

/*
 * Read/write one whole data block. `buf` must be at least EXOFS_BLOCK_SIZE
 * bytes and in the LibOS VA window. Returns 0, -EXO_EINVAL for a block index
 * past the end of the volume, or a device error.
 */
int exofs_read_block(uint32_t blk, void *buf);
int exofs_write_block(uint32_t blk, const void *buf);

/* Mark the FAT sector holding entry `idx` as needing writeback. Called by
 * whoever writes v->fat[idx]; separated from the write itself so a loop that
 * touches many consecutive entries does not repeat the division. */
void exofs_fat_mark_dirty(exofs_volume_t *v, uint32_t idx);

#endif /* EXOFS_INTERNAL_H */
