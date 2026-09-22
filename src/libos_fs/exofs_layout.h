#ifndef EXOFS_LAYOUT_H
#define EXOFS_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

/*
 * exofs_layout.h — the ExoFS on-disk format (SCRUM-189).
 *
 * ExoFS is the team's CS314 FAT-like filesystem (the `cfat` project) ported
 * to run as ExoDoom LibOS code over exo_disk_read/exo_disk_write, per the
 * 2026-09-19 storage decision recorded on SCRUM-189. The *shape* is cfat's:
 * a FAT of fixed-size blocks, directories that are chains of fixed-size
 * entries, `.`/`..`, block 0 as the root. Three things are deliberately not
 * cfat's, and each one is called out where it appears below: the FAT is 32
 * bits wide, names are variable-length through an indirection, and a volume
 * begins with a superblock.
 *
 * WHY THIS IS ITS OWN HEADER. Everything here is plain old data — structs,
 * constants, and _Static_asserts that pin their sizes. No function
 * declarations, and nothing included beyond <stdint.h>/<stddef.h>. That is
 * so SCRUM-190's host-side image builder can compile against this exact file
 * rather than restating the format in a script, which is the usual way two
 * halves of an on-disk format drift apart. It follows that nothing ExoDoom-
 * specific may be added here: the moment this header needs exo_syscall.h or
 * libos_heap.h, the builder can no longer use it. Put that in exofs.h or in
 * the .c files instead.
 *
 * ENDIANNESS. Little-endian, unaligned-safe by construction: every field is
 * explicit-width and every struct is laid out so that no field straddles its
 * own natural alignment. x86-only, so no byte-swapping layer exists. A
 * big-endian reader would need one; there isn't going to be one.
 *
 * GEOMETRY. A volume starts at a caller-supplied base LBA rather than at
 * LBA 0:
 *
 *     base_lba + 0                       superblock (exactly one sector)
 *     base_lba + 1                       FAT, `fat_blocks` sectors
 *     base_lba + 1 + fat_blocks          data region, `total_blocks` blocks
 *
 * The base is a parameter and not a constant because the disk is shared.
 * tests/kernel/test_ata_k.c already owns LBA 2048/2049 of the scratch image
 * and tests/kernel/test_syscall_disk_k.c owns LBA 3072; a volume that always
 * started at 0 would scribble on both. It also means SCRUM-190 can place a
 * volume at a partition-like offset without a format change.
 */

/* Block size == sector size. Keeping these equal is what lets a block index
 * be turned into an LBA with one addition instead of a multiply-and-split,
 * and exo_disk_read/exo_disk_write only speak 512-byte sectors anyway
 * (docs/syscall_spec.md §3.2 #27). */
#define EXOFS_BLOCK_SIZE     512u

/* 'XOFS' little-endian. Checked on mount; a volume whose first four bytes
 * are anything else is refused rather than interpreted. This is the whole
 * reason the superblock exists — cfat had no such check and would happily
 * read a blank or foreign disk as a FAT. */
#define EXOFS_MAGIC          0x53464F58u
#define EXOFS_VERSION        1u

/* ---- FAT ---------------------------------------------------------------
 *
 * 32-bit entries, not cfat's `unsigned short`. A 16-bit FAT caps a volume at
 * 65535 blocks = 32 MiB, and SCRUM-190 has to fit a ~28 MB freedoom2.wad on
 * this filesystem once the multiboot WAD module is retired — close enough to
 * the ceiling that the first larger IWAD would hit it. Four bytes per entry
 * costs 64 KiB of FAT on a 16384-block volume, which is read into RAM whole
 * at mount either way.
 *
 * EXOFS_BLOCK_FREE is 0, which would be ambiguous with "block 0" if block 0
 * could ever be the *target* of a chain link. It cannot: block 0 is the root
 * directory, and the root is never linked into another chain (nothing
 * allocates it, and it has no parent to be chained from). That is the same
 * invariant cfat relied on, written down.
 */
typedef uint32_t exofs_fat_t;

#define EXOFS_BLOCK_FREE     0u            /* unallocated                  */
#define EXOFS_BLOCK_EOC      0xFFFFFFFFu   /* last block of a chain        */
#define EXOFS_NO_BLOCK       0xFFFFFFFFu   /* "no chain at all" in a dirent */

#define EXOFS_FAT_PER_BLOCK  (EXOFS_BLOCK_SIZE / sizeof(exofs_fat_t))  /* 128 */

/* The root directory's block index. Fixed, not stored-and-trusted: the
 * superblock carries it too, but mount validates it equals this. */
#define EXOFS_ROOT_BLOCK     0u

/* ---- Superblock --------------------------------------------------------- */

typedef struct exofs_super {
    uint32_t magic;          /* EXOFS_MAGIC                                 */
    uint16_t version;        /* EXOFS_VERSION                               */
    uint16_t block_size;     /* EXOFS_BLOCK_SIZE; validated, not trusted    */
    uint32_t total_blocks;   /* data blocks the FAT covers                  */
    uint32_t fat_blocks;     /* sectors the FAT itself occupies             */
    uint32_t data_lba;       /* absolute LBA of data block 0                */
    uint32_t root_block;     /* EXOFS_ROOT_BLOCK                            */

    /* First block of the name-area chain, or EXOFS_NO_BLOCK on a volume
     * that holds no names yet. It has to live here rather than be derived:
     * the name area is a chain of ordinary data blocks, and nothing else on
     * the volume records where it starts. Walking every directory to
     * rediscover it at mount would be both slow and circular, since
     * resolving a directory entry needs the name area already. */
    uint32_t name_head;

    uint8_t  pad[EXOFS_BLOCK_SIZE - 28];
} exofs_super_t;

_Static_assert(sizeof(exofs_super_t) == EXOFS_BLOCK_SIZE,
               "exofs_super_t must be exactly one block");

/* ---- Directory entries --------------------------------------------------
 *
 * Fixed 32 bytes, so exactly 16 per block and an entry's position is
 * (block, index) arithmetic rather than a scan. Fixed size is precisely why
 * the name is an indirection: cfat inlined `char name[11]`, which is both
 * the 8.3 limit SCRUM-189 exists to remove and the reason its entries could
 * stay fixed-size at all.
 *
 * A name lives as a record in the name area (see below); the entry stores
 * where. Three fields rather than one offset because a name record is
 * addressed as (block, byte offset) and the length is worth having without
 * a second read.
 */
typedef struct exofs_dirent {
    uint32_t name_block;     /* data block holding this entry's name record */
    uint16_t name_off;       /* byte offset of the record within that block */
    uint16_t name_len;       /* name length in bytes; no NUL is stored      */
    uint32_t first_block;    /* first data/dir block, or EXOFS_NO_BLOCK     */
    uint32_t size;           /* file size in bytes; 0 for a directory       */
    uint32_t mtime;          /* see "Time" below                            */
    uint32_t ctime;
    uint16_t attributes;     /* EXOFS_ATTR_*                                */
    uint8_t  flags;          /* EXOFS_ENT_*                                 */

    /* The fields above come to 27 bytes, which the compiler would round to
     * 28. Padding to 32 is deliberate and not cosmetic: 512/32 = 16 entries
     * per block exactly, whereas 28 gives 18 entries and 8 wasted bytes at
     * the end of every directory block — a partial entry straddling nothing,
     * which every scan would then have to special-case. Must be zero; it is
     * where a future field (a second timestamp word, an owner id) goes
     * without a format version bump, so code must not assume it stays zero
     * once written by a newer writer. */
    uint8_t  reserved[5];
} exofs_dirent_t;

_Static_assert(sizeof(exofs_dirent_t) == 32,
               "exofs_dirent_t must be 32 bytes: the entries-per-block "
               "arithmetic and the on-disk format both depend on it");

#define EXOFS_ENTS_PER_BLOCK (EXOFS_BLOCK_SIZE / sizeof(exofs_dirent_t))  /* 16 */

/* attributes */
#define EXOFS_ATTR_FILE      0x0001u
#define EXOFS_ATTR_DIRECTORY 0x0002u

/* flags.
 *
 * EXOFS_ENT_LAST terminates a directory early, the way cfat's `isLast` did.
 * EXOFS_ENT_FREE has no cfat equivalent — cfat never reclaimed an entry
 * slot, so a create-delete-create cycle grew the directory forever. A free
 * slot is reused before the directory is extended. */
#define EXOFS_ENT_LAST       0x01u
#define EXOFS_ENT_FREE       0x02u

/* ---- Name area ----------------------------------------------------------
 *
 * Names live in ordinary FAT-allocated blocks, chained through the FAT from
 * the superblock's name_head — there is no separate allocator and no
 * reserved region, so a volume that stores few names does not pay for a big
 * one. A record is:
 *
 *     uint16_t hdr;          low 15 bits = CAPACITY, high bit = free
 *     uint8_t  bytes[cap];   the name, no NUL, left-justified in `cap`
 *
 * The header holds the record's capacity, not the name's length. The length
 * is in the referring exofs_dirent_t's name_len, where it already had to be
 * — and keeping capacity here is what lets a scan skip a record without
 * caring whether it is live or free, and lets a free record be reused by a
 * shorter name without its footprint changing under the scan. A record whose
 * capacity exceeds the name in it is split on reuse when the leftover is big
 * enough to hold a record of its own.
 *
 * A header of 0 is not a zero-capacity record: it means "unused space from
 * here to the end of the block". That falls out of exofs_fat_alloc() zeroing
 * every block it hands out, so a fresh name block needs no initialisation.
 *
 * Records never straddle a block boundary, so reading a name is always one
 * block read. That caps a name at EXOFS_BLOCK_SIZE - 2 bytes by construction;
 * the API advertises EXOFS_MAX_NAME (255, POSIX NAME_MAX) instead, which
 * leaves room to raise the limit later without a format change.
 *
 * Freeing sets EXOFS_NAME_FREE_BIT and coalesces with adjacent free records
 * rather than compacting the block; allocation first-fits over free records
 * before bumping into unused space. That matters for SCRUM-104: Doom's
 * save-file rotation renames constantly, and a bump-only name area would
 * grow without bound.
 */
#define EXOFS_MAX_NAME       255u
#define EXOFS_NAME_HDR_SIZE  2u
#define EXOFS_NAME_FREE_BIT  0x8000u
#define EXOFS_NAME_CAP_MASK  0x7FFFu

_Static_assert(EXOFS_MAX_NAME + EXOFS_NAME_HDR_SIZE <= EXOFS_BLOCK_SIZE,
               "a name record must fit in one block: exofs_name.c reads a "
               "name with a single block read and never spans");

/* ---- Time ---------------------------------------------------------------
 *
 * ctime/mtime hold exo_get_ticks() — monotonic milliseconds since boot, not
 * a wall-clock date. cfat's datetime.c packed a real FAT date from
 * localtime(); there is no clock here to ask, and inventing a fake epoch
 * would make these fields look more meaningful than they are. They order
 * writes within one boot and nothing more. A volume carried across a reboot
 * has timestamps from a previous boot's tick count, which is why no code
 * should compare them across a mount.
 */

#endif /* EXOFS_LAYOUT_H */
