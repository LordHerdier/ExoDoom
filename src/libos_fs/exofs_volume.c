/*
 * exofs_volume.c — superblock, format, mount, FAT cache (SCRUM-189).
 *
 * The cfat equivalents are fsops.c (createfs/loadfs/formatfs) and globals.c
 * (the mmap'd base pointer and the FAT/blocks pointers derived from it).
 * Neither survives intact, for the same underlying reason: cfat mapped a
 * 10 MB file and let every other module reach the FAT and the data blocks
 * through raw pointers into that mapping. There is no mmap here. The FAT
 * becomes an explicit in-RAM array written back a sector at a time, and data
 * blocks become explicit transfers.
 *
 * What is new relative to cfat is the superblock and everything that
 * follows from it: geometry is computed once at format time and recorded,
 * rather than being compile-time constants every reader has to agree on by
 * convention, and mount validates rather than assumes.
 */

#include "exofs.h"
#include "exofs_internal.h"
#include "exofs_layout.h"
#include "exofs_blockdev.h"
#include "exofs_dir.h"      /* exofs_dir_init_root — see step 4 of format() */

#include "exo_errno.h"
#include "libos_heap.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

/* The one mounted volume. See exofs.h's "one volume at a time" note for why
 * this is a static and not a handle the caller carries. */
static exofs_volume_t g_vol;

exofs_volume_t *exofs_vol(void)
{
    return g_vol.mounted ? &g_vol : NULL;
}

int exofs_is_mounted(void)
{
    return g_vol.mounted;
}

/* ---- Geometry -----------------------------------------------------------
 *
 * Given `total_sectors` starting at the superblock, how many data blocks fit
 * once the FAT that describes them is also paid for?
 *
 *   1 (superblock) + fat_blocks + total_blocks <= total_sectors
 *   fat_blocks = ceil(total_blocks / EXOFS_FAT_PER_BLOCK)
 *
 * Dropping the ceiling and solving gives total_blocks <= remaining * P /
 * (P + 1) with P = EXOFS_FAT_PER_BLOCK, which is the starting estimate
 * below. The ceiling can make that estimate one block too generous, so it is
 * checked and walked down rather than trusted — cheaper and more obviously
 * correct than a closed form that has to be right about the rounding.
 */
static int compute_geometry(uint32_t total_sectors, uint32_t *total_blocks_out,
                            uint32_t *fat_blocks_out)
{
    if (total_sectors < EXOFS_MIN_SECTORS) return -EXO_EINVAL;

    uint32_t remaining = total_sectors - 1u;    /* less the superblock */

    /* 64-bit intermediate: remaining * EXOFS_FAT_PER_BLOCK overflows 32 bits
     * for a volume above ~17 GB, which nothing here would otherwise stop. */
    uint64_t est = ((uint64_t)remaining * EXOFS_FAT_PER_BLOCK)
                 / (EXOFS_FAT_PER_BLOCK + 1u);

    if (est > EXOFS_MAX_BLOCKS) est = EXOFS_MAX_BLOCKS;

    uint32_t total_blocks = (uint32_t)est;

    while (total_blocks > 0) {
        uint32_t fat_blocks =
            (total_blocks + EXOFS_FAT_PER_BLOCK - 1u) / EXOFS_FAT_PER_BLOCK;

        if ((uint64_t)1u + fat_blocks + total_blocks <= total_sectors) {
            *total_blocks_out = total_blocks;
            *fat_blocks_out   = fat_blocks;
            return 0;
        }
        total_blocks--;
    }

    return -EXO_EINVAL;
}

/* ---- Block helpers ------------------------------------------------------ */

uint32_t exofs_block_lba(const exofs_volume_t *v, uint32_t blk)
{
    return v->data_lba + blk;
}

int exofs_read_block(uint32_t blk, void *buf)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;
    if (blk >= v->total_blocks) return -EXO_EINVAL;

    return exofs_bdev_read(exofs_block_lba(v, blk), buf, 1);
}

int exofs_write_block(uint32_t blk, const void *buf)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;
    if (blk >= v->total_blocks) return -EXO_EINVAL;

    return exofs_bdev_write(exofs_block_lba(v, blk), buf, 1);
}

/* ---- FAT writeback ------------------------------------------------------ */

void exofs_fat_mark_dirty(exofs_volume_t *v, uint32_t idx)
{
    uint32_t sector = idx / EXOFS_FAT_PER_BLOCK;
    if (sector >= v->fat_blocks) return;   /* not a real FAT sector */

    v->fat_dirty[sector / 8u] |= (uint8_t)(1u << (sector % 8u));
}

/*
 * Write back every dirty FAT sector.
 *
 * Adjacent dirty sectors are coalesced into one transfer: allocating a chain
 * of N blocks dirties a run of consecutive entries far more often than a
 * scattered set, and exofs_bdev_write() already chunks anything longer than
 * the syscall's cap, so there is no length to be careful about here.
 */
static int fat_flush(exofs_volume_t *v)
{
    uint32_t i = 0;

    while (i < v->fat_blocks) {
        int dirty = (v->fat_dirty[i / 8u] >> (i % 8u)) & 1u;
        if (!dirty) { i++; continue; }

        uint32_t run = 0;
        while (i + run < v->fat_blocks &&
               ((v->fat_dirty[(i + run) / 8u] >> ((i + run) % 8u)) & 1u)) {
            run++;
        }

        const uint8_t *src =
            (const uint8_t *)v->fat + (size_t)i * EXOFS_BLOCK_SIZE;

        int rc = exofs_bdev_write(v->fat_lba + i, src, run);
        if (rc < 0) return rc;

        /* Clear only after the write succeeded: a failed flush must leave
         * the sectors dirty so a later sync retries them, rather than
         * quietly declaring the disk up to date. */
        for (uint32_t k = 0; k < run; k++) {
            uint32_t s = i + k;
            v->fat_dirty[s / 8u] &= (uint8_t)~(1u << (s % 8u));
        }

        i += run;
    }

    return 0;
}

int exofs_sync(void)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL) return -EXO_EINVAL;

    return fat_flush(v);
}

/* ---- Teardown ----------------------------------------------------------- */

static void release_volume(void)
{
    libos_heap_free(g_vol.fat);
    libos_heap_free(g_vol.fat_dirty);
    libos_heap_free(g_vol.scratch);
    memset(&g_vol, 0, sizeof(g_vol));
}

void exofs_unmount(void)
{
    if (!g_vol.mounted) return;

    (void)fat_flush(&g_vol);   /* see exofs.h on why the status is dropped */
    release_volume();
}

/* ---- Format -------------------------------------------------------------
 *
 * Writes three things, in an order chosen so an interrupted format does not
 * leave a mountable-but-wrong volume: the FAT and root block first, the
 * superblock last. Until that final sector lands, exofs_mount() sees no
 * magic and refuses — which is the honest answer for a volume whose FAT was
 * only half written.
 */
int exofs_format(uint32_t base_lba, uint32_t total_sectors)
{
    /* Formatting under a live mount would leave that mount describing a
     * volume that no longer exists — its cached FAT, its geometry and its
     * name-chain head all stale. Refuse rather than corrupt. */
    if (g_vol.mounted) return -EXO_EBUSY;

    uint32_t total_blocks = 0, fat_blocks = 0;
    int rc = compute_geometry(total_sectors, &total_blocks, &fat_blocks);
    if (rc < 0) return rc;

    uint32_t fat_lba  = base_lba + 1u;
    uint32_t data_lba = fat_lba + fat_blocks;

    /* One block-sized zeroed staging buffer, reused for every sector this
     * function writes. In the LibOS window, like every disk buffer. */
    uint8_t *zero = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (zero == NULL) return -EXO_ENOMEM;
    memset(zero, 0, EXOFS_BLOCK_SIZE);

    /* 1. The FAT, every entry EXOFS_BLOCK_FREE (0) — which is why a zeroed
     *    buffer is the whole of it — except the root's own entry. */
    for (uint32_t i = 0; i < fat_blocks; i++) {
        rc = exofs_bdev_write(fat_lba + i, zero, 1);
        if (rc < 0) goto out;
    }

    /* The root directory occupies block 0 and is a one-block chain, so
     * FAT[0] is end-of-chain rather than free. It lives in FAT sector 0. */
    {
        exofs_fat_t *fat0 = (exofs_fat_t *)zero;
        fat0[0] = EXOFS_BLOCK_EOC;
        rc = exofs_bdev_write(fat_lba, zero, 1);
        if (rc < 0) goto out;
        fat0[0] = 0;                        /* back to all-zero for reuse */
    }

    /* 2. The root directory block itself, zeroed. Its "." and ".." are
     *    written at the end of this function — see step 4.
     *
     *    exofs_bdev_write() directly rather than exofs_write_block(): that
     *    helper resolves a block index through the *mounted* volume, and
     *    format() runs with nothing mounted. It has the geometry in hand
     *    from compute_geometry() instead. */
    rc = exofs_bdev_write(data_lba + EXOFS_ROOT_BLOCK, zero, 1);
    if (rc < 0) goto out;

    /* 3. The superblock, last — see this function's header comment. */
    {
        exofs_super_t *sb = (exofs_super_t *)zero;
        memset(sb, 0, EXOFS_BLOCK_SIZE);
        sb->magic        = EXOFS_MAGIC;
        sb->version      = (uint16_t)EXOFS_VERSION;
        sb->block_size   = (uint16_t)EXOFS_BLOCK_SIZE;
        sb->total_blocks = total_blocks;
        sb->fat_blocks   = fat_blocks;
        sb->data_lba     = data_lba;
        sb->root_block   = EXOFS_ROOT_BLOCK;
        sb->name_head    = EXOFS_NO_BLOCK;   /* no names on a fresh volume */

        rc = exofs_bdev_write(base_lba, sb, 1);
        if (rc < 0) goto out;
    }

    /*
     * 4. The root's "." and "..".
     *
     * This needs a mounted volume, because creating a directory entry
     * allocates a name record and that needs the FAT cache and the
     * superblock's name_head — so format mounts what it has just written,
     * bootstraps the root, syncs and unmounts. cfat's createfs() called
     * createRootDirectory() at the same point and for the same reason.
     *
     * It has to be last: everything above is what makes the volume
     * mountable in the first place.
     */
    rc = exofs_mount(base_lba);
    if (rc < 0) goto out;

    rc = exofs_dir_init_root(&g_vol);
    if (rc == 0) rc = fat_flush(&g_vol);

    /* Unmount unconditionally: format's contract is that it leaves nothing
     * mounted, whether or not the bootstrap succeeded. */
    exofs_unmount();

out:
    libos_heap_free(zero);
    return rc;
}

/* ---- Mount -------------------------------------------------------------- */

/*
 * Everything the superblock claims is checked against something else, not
 * just against itself: a corrupted sector that happens to start with the
 * right four bytes must not produce a mounted volume whose data_lba points
 * anywhere at all. These are the checks a blank disk, a foreign filesystem,
 * and a truncated format each fail.
 */
static int validate_super(const exofs_super_t *sb, uint32_t base_lba)
{
    if (sb->magic != EXOFS_MAGIC)                 return -EXO_EINVAL;
    if (sb->version != EXOFS_VERSION)             return -EXO_EINVAL;
    if (sb->block_size != EXOFS_BLOCK_SIZE)       return -EXO_EINVAL;
    if (sb->root_block != EXOFS_ROOT_BLOCK)       return -EXO_EINVAL;

    if (sb->total_blocks == 0)                    return -EXO_EINVAL;
    if (sb->total_blocks > EXOFS_MAX_BLOCKS)      return -EXO_EINVAL;

    /* The FAT must be exactly big enough for the block count it describes —
     * not merely large enough, since a too-large fat_blocks would push
     * data_lba past where format() put the data. */
    uint32_t want_fat =
        (sb->total_blocks + EXOFS_FAT_PER_BLOCK - 1u) / EXOFS_FAT_PER_BLOCK;
    if (sb->fat_blocks != want_fat)               return -EXO_EINVAL;

    /* And the data region must start immediately after it. */
    if (sb->data_lba != base_lba + 1u + sb->fat_blocks) return -EXO_EINVAL;

    /* The name chain either does not exist yet or starts at a real block.
     * Anything else would have exofs_name.c walking off the volume. */
    if (sb->name_head != EXOFS_NO_BLOCK &&
        sb->name_head >= sb->total_blocks) return -EXO_EINVAL;

    return 0;
}

int exofs_mount(uint32_t base_lba)
{
    if (g_vol.mounted) return -EXO_EBUSY;

    uint8_t *buf = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (buf == NULL) return -EXO_ENOMEM;

    int rc = exofs_bdev_read(base_lba, buf, 1);
    if (rc < 0) goto out_buf;

    const exofs_super_t *sb = (const exofs_super_t *)buf;
    rc = validate_super(sb, base_lba);
    if (rc < 0) goto out_buf;

    memset(&g_vol, 0, sizeof(g_vol));
    g_vol.base_lba     = base_lba;
    g_vol.fat_lba      = base_lba + 1u;
    g_vol.fat_blocks   = sb->fat_blocks;
    g_vol.data_lba     = sb->data_lba;
    g_vol.total_blocks = sb->total_blocks;
    g_vol.root_block   = sb->root_block;
    g_vol.name_head    = sb->name_head;

    /* Sized to whole sectors, not to the entry count: the FAT is read with
     * one sector-granular transfer and the last sector is usually only
     * partly used. See exofs_internal.h's note on this field. */
    size_t fat_bytes = (size_t)g_vol.fat_blocks * EXOFS_BLOCK_SIZE;
    g_vol.fat = libos_heap_alloc(fat_bytes);
    if (g_vol.fat == NULL) { rc = -EXO_ENOMEM; goto out_partial; }

    g_vol.fat_dirty_bytes = (g_vol.fat_blocks + 7u) / 8u;
    g_vol.fat_dirty = libos_heap_alloc(g_vol.fat_dirty_bytes);
    if (g_vol.fat_dirty == NULL) { rc = -EXO_ENOMEM; goto out_partial; }
    memset(g_vol.fat_dirty, 0, g_vol.fat_dirty_bytes);

    g_vol.scratch = libos_heap_alloc(EXOFS_BLOCK_SIZE);
    if (g_vol.scratch == NULL) { rc = -EXO_ENOMEM; goto out_partial; }

    rc = exofs_bdev_read(g_vol.fat_lba, g_vol.fat, g_vol.fat_blocks);
    if (rc < 0) goto out_partial;

    /* The root's FAT entry is the one thing format() writes that mount can
     * cross-check, and a volume whose root chain is free is not one any
     * traversal could survive. */
    if (g_vol.fat[EXOFS_ROOT_BLOCK] == EXOFS_BLOCK_FREE) {
        rc = -EXO_EINVAL;
        goto out_partial;
    }

    g_vol.mounted = 1;
    rc = 0;
    goto out_buf;

out_partial:
    /* Not release_volume(): that one asserts a mounted volume's invariants
     * by zeroing the whole struct, which is right here too, but the
     * mounted flag was never set so unmount() must not be the path. */
    libos_heap_free(g_vol.fat);
    libos_heap_free(g_vol.fat_dirty);
    libos_heap_free(g_vol.scratch);
    memset(&g_vol, 0, sizeof(g_vol));

out_buf:
    libos_heap_free(buf);
    return rc;
}

/*
 * Rewrite the superblock from the mounted volume's state.
 *
 * Only exofs_name.c calls this, and only when the name chain's head changes
 * — which happens exactly once per volume, the first time a name is stored.
 * A superblock rewrite is not cheap in the way a FAT mark is (it is an
 * immediate sector write, not a dirty bit), which is fine at that
 * frequency and would not be if anything else were recorded here.
 */
int exofs_super_update(exofs_volume_t *v)
{
    if (v == NULL || !v->mounted) return -EXO_EINVAL;
    if (v->scratch == NULL)       return -EXO_EINVAL;

    exofs_super_t *sb = (exofs_super_t *)v->scratch;
    memset(sb, 0, EXOFS_BLOCK_SIZE);
    sb->magic        = EXOFS_MAGIC;
    sb->version      = (uint16_t)EXOFS_VERSION;
    sb->block_size   = (uint16_t)EXOFS_BLOCK_SIZE;
    sb->total_blocks = v->total_blocks;
    sb->fat_blocks   = v->fat_blocks;
    sb->data_lba     = v->data_lba;
    sb->root_block   = v->root_block;
    sb->name_head    = v->name_head;

    return exofs_bdev_write(v->base_lba, sb, 1);
}

int exofs_geometry(exofs_geometry_t *out)
{
    exofs_volume_t *v = exofs_vol();
    if (v == NULL || out == NULL) return -EXO_EINVAL;

    out->base_lba     = v->base_lba;
    out->total_blocks = v->total_blocks;
    out->fat_blocks   = v->fat_blocks;
    out->data_lba     = v->data_lba;
    out->root_block   = v->root_block;
    return 0;
}
