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

/* See exofs_volume_t::open_ent_block below. */
#define EXOFS_MAX_OPEN_HANDLES 8u

/* See exofs_volume_t::name_room below. A real entry is at most
 * EXOFS_BLOCK_SIZE - EXOFS_NAME_HDR_SIZE, so this cannot be mistaken for
 * one. */
#define EXOFS_NAME_ROOM_UNKNOWN 0xFFFFu

typedef struct exofs_volume {
    int      mounted;

    /* Geometry, as recorded in and validated against the superblock. */
    uint32_t base_lba;       /* LBA of the superblock                     */
    uint32_t fat_lba;        /* base_lba + 1                              */
    uint32_t fat_blocks;     /* sectors the FAT occupies                  */
    uint32_t data_lba;       /* absolute LBA of data block 0              */
    uint32_t total_blocks;   /* data blocks the FAT covers                */
    uint32_t root_block;     /* EXOFS_ROOT_BLOCK                          */

    /* Head of the name-area chain, mirroring the superblock field. Changes
     * exactly once per volume — the first time a name is stored — and
     * exofs_super_update() writes it back when it does. */
    uint32_t name_head;

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

    /*
     * What each block of the name chain can still take (SCRUM-223).
     *
     * name_room[i] is the longest name the i-th block of the chain has room
     * for, or EXOFS_NAME_ROOM_UNKNOWN when that block has not been looked at
     * since mount. It is the name area's counterpart to next_free_hint
     * above, and it is a table rather than one number because names are not
     * one size: a block with no room for a 200-byte name may still have room
     * for a 5-byte one, so "the first block with room" is a different block
     * for every length asked for. One hint either strands the space it has
     * moved past or has to rescan it, which is the cost this exists to
     * remove. exofs_name.c has the allocation loop that consults it.
     *
     * NOT A CACHE OF ANYTHING ON DISK. It holds no block contents, is never
     * written back and has no dirty state, so the top of this file is still
     * right that the FAT's bitmap is the only dirty-state invariant. All it
     * ever answers is "is this block worth reading?", and it can only be
     * unhelpful in the harmless direction: UNKNOWN -- or an index past
     * name_room_cap, when the heap could not supply a bigger table -- means
     * "read it and see", which is what every allocation did for every block
     * before this existed.
     *
     * From libos_heap_alloc(), freed at unmount. NULL until the first name
     * is stored after a mount; 2 bytes per name block after that.
     */
    uint16_t *name_room;
    uint32_t  name_room_cap;

    /*
     * Data-block transfers since mount, counted in exofs_read_block() and
     * exofs_write_block(). Nothing in the filesystem reads these. They exist
     * so that "this operation re-reads the whole chain" can be a failing
     * assertion in tests/kernel/test_exofs_k.c instead of a remark in a code
     * review: SCRUM-223, -224 and -225 were all found by reading, and each
     * is one refactor away from coming back unnoticed.
     */
    uint32_t stat_block_reads;
    uint32_t stat_block_writes;

    /*
     * Directory-entry refs of every exofs_file_t currently open (SCRUM-189
     * review fix). There is no descriptor table here (exofs.h says so), but
     * exofs_open()'s O_TRUNC path and exofs_unlink() both free a file's block
     * chain, and a second handle already open on that same entry caches
     * first_block/cur_block from before the free — the next read or write
     * through it then walks into whatever the allocator has since handed to
     * an unrelated file. Small and fixed, like every other concurrency-shaped
     * limit in this codebase (CONTEXT_MAX, LIBOS_LAUNCH_MAX_*_PAGES).
     * EXOFS_NO_BLOCK marks an empty slot; entries are unordered and may
     * repeat a (block, index) pair once per handle open on it.
     */
    uint32_t open_ent_block[EXOFS_MAX_OPEN_HANDLES];
    uint16_t open_ent_index[EXOFS_MAX_OPEN_HANDLES];
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

/* Rewrite the superblock from `v`. See the definition in exofs_volume.c for
 * why this is a rare, immediate write rather than something batched. */
int exofs_super_update(exofs_volume_t *v);

/* ---- Open-handle tracking -------------------------------------------------
 *
 * See exofs_volume_t::open_ent_block for why this exists. Registering never
 * fails except when the small fixed table is full (-EXO_ENFILE); a caller
 * that gets that back has not opened its handle and must not proceed as if
 * it had. `exofs_handle_is_open()` deliberately does not distinguish "one
 * other handle" from "several" — the callers that consult it only ever need
 * "any at all".
 */
int  exofs_handle_is_open(exofs_volume_t *v, uint32_t block, uint16_t index);
int  exofs_handle_register(exofs_volume_t *v, uint32_t block, uint16_t index);
void exofs_handle_unregister(exofs_volume_t *v, uint32_t block, uint16_t index);

#endif /* EXOFS_INTERNAL_H */
