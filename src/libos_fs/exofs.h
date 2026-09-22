#ifndef EXOFS_H
#define EXOFS_H

#include <stdint.h>

#include "exofs_layout.h"

/*
 * exofs.h — ExoFS public API (SCRUM-189).
 *
 * ExoFS is the team's cfat FAT-like filesystem ported to run as ExoDoom
 * LibOS code over exo_disk_read/exo_disk_write. It is a *library*, not a
 * kernel subsystem: the exokernel knows sectors, and this code is what
 * decides that some of those sectors are a file. See exofs_layout.h for the
 * on-disk format and exofs_blockdev.h for how it reaches the disk.
 *
 * ERRORS. Every call returns 0 (or a non-negative count) on success, or a
 * negative EXO_E* code from src/exo_errno.h — the same convention the
 * syscalls themselves use (docs/syscall_spec.md §3.1). Nothing here calls
 * exit(), unlike cfat, which ended the process on a full disk or a missing
 * file; a library that terminates its caller cannot be used by a shell or by
 * Doom.
 *
 * ONE VOLUME AT A TIME. The mounted volume is a single static state, so
 * there is no handle to pass around and no second mount. That is not a
 * simplification that needs undoing later: the disk binding (SCRUM-188) is
 * exclusive, so only one context can reach the disk at all, and that context
 * has one disk to mount.
 *
 * THREADING. None. There is no preemption inside a LibOS today, and no lock
 * here would help if there were — the FAT cache would need one too.
 *
 * The file/directory half of this API (open/read/write/mkdir/readdir/...)
 * lands in later steps of SCRUM-189 and is declared here as it arrives.
 */

/*
 * Lay down a fresh, empty volume of `total_sectors` sectors starting at
 * absolute LBA `base_lba`: superblock, a FAT with every block free, and a
 * zeroed root directory block.
 *
 * The caller supplies `total_sectors` because there is no disk-capacity
 * syscall — ata_init() issues IDENTIFY, which carries the sector count, but
 * src/ata.c does not surface it. Geometry is derived from that number and
 * recorded in the superblock, so exofs_mount() needs only the base.
 *
 * Destroys whatever was in [base_lba, base_lba + total_sectors). Returns 0,
 * -EXO_EINVAL (`total_sectors` below EXOFS_MIN_SECTORS, or a volume whose
 * FAT would exceed EXOFS_MAX_BLOCKS), -EXO_ENOMEM, or any error
 * exofs_bdev_write() reports (-EXO_EBUSY if the disk binding is not held,
 * -EXO_ENODEV, -EXO_EIO).
 *
 * Does not leave the volume mounted; call exofs_mount() afterwards.
 */
int exofs_format(uint32_t base_lba, uint32_t total_sectors);

/*
 * Mount the volume at `base_lba`: read and validate the superblock, then
 * read the whole FAT into memory.
 *
 * Returns 0, -EXO_EINVAL (no ExoFS superblock there, an unsupported
 * version, or internally inconsistent geometry — this is what a blank or
 * foreign disk gets, and the reason the superblock exists), -EXO_EBUSY (a
 * volume is already mounted), -EXO_ENOMEM, or a device error.
 */
int exofs_mount(uint32_t base_lba);

/*
 * Write back any FAT sectors modified since the last sync.
 *
 * Structural operations sync as they go, so this is for callers that want a
 * known-durable point rather than a routine step. Returns 0, -EXO_EINVAL if
 * nothing is mounted, or a device error.
 */
int exofs_sync(void);

/*
 * Sync and release the mounted volume. Safe to call when nothing is
 * mounted. Any sync error is discarded — there is nothing a caller could do
 * about it at this point and no way to report it that would not make the
 * common path harder; call exofs_sync() first if the answer matters.
 */
void exofs_unmount(void);

/* Whether a volume is currently mounted. */
int exofs_is_mounted(void);

/* Geometry of the mounted volume, for callers that want to report free
 * space or size a transfer. Fills `out` and returns 0, or -EXO_EINVAL if
 * nothing is mounted. */
typedef struct exofs_geometry {
    uint32_t base_lba;
    uint32_t total_blocks;   /* data blocks the FAT covers      */
    uint32_t fat_blocks;     /* sectors the FAT occupies        */
    uint32_t data_lba;       /* absolute LBA of data block 0    */
    uint32_t root_block;     /* EXOFS_ROOT_BLOCK                */
} exofs_geometry_t;

int exofs_geometry(exofs_geometry_t *out);

/* ---- Directories --------------------------------------------------------
 *
 * Paths are absolute — they must start with '/'. See docs/filesystem.md §7
 * for why there is no working directory here.
 */

/*
 * Create the directory `path`. Its "." and ".." are written before it is
 * linked into its parent, so a directory is never visible in a half-made
 * state.
 *
 * Returns 0, -EXO_EEXIST if the name is taken, -EXO_ENOENT if a parent
 * component is missing, -EXO_ENOTDIR if one is not a directory,
 * -EXO_EINVAL for a malformed path or an over-long component,
 * -EXO_ENOSPC, or a device error.
 */
int exofs_mkdir(const char *path);

/*
 * Remove the empty directory `path`.
 *
 * Returns 0, -EXO_ENOENT, -EXO_ENOTDIR if `path` is not a directory,
 * -EXO_ENOTEMPTY if it holds anything besides "." and "..", -EXO_EBUSY for
 * the root (which has no parent to be removed from), or a device error.
 */
int exofs_rmdir(const char *path);

/* One entry, as reported by exofs_readdir(). */
typedef struct exofs_dirinfo {
    char     name[EXOFS_MAX_NAME + 1];
    uint32_t size;          /* bytes, for a file; 0 for a directory */
    uint16_t attributes;    /* EXOFS_ATTR_*                         */
} exofs_dirinfo_t;

/*
 * An open directory. Caller-allocated and opaque; there is no descriptor
 * table here, because the layer with an ABI to defend (exo_file_*,
 * SCRUM-44) is where one belongs.
 */
typedef struct exofs_dir {
    int      open;
    uint32_t head;
    /* Iteration state. Deliberately not exofs_dir_iter_t by name: exofs.h
     * is the public header and must not drag in the internal ones. The
     * fields mirror it exactly and exofs_dir.c does the conversion. */
    uint32_t block;
    uint16_t index;
    uint32_t steps;
    int      done;
} exofs_dir_t;

/*
 * Open `path` for reading. Returns 0, -EXO_ENOENT, -EXO_ENOTDIR if `path`
 * is not a directory, or a negative error.
 */
int exofs_opendir(const char *path, exofs_dir_t *out);

/*
 * Read the next entry into `out`.
 *
 * Returns 1 with an entry, 0 at the end of the directory, or a negative
 * error. "." and ".." ARE reported, the way POSIX readdir does — they are
 * ordinary entries here (exofs_dir.h), a shell listing wants them, and
 * filtering them would cost a name comparison on every single iteration to
 * hide something the caller can skip for free.
 */
int exofs_readdir(exofs_dir_t *d, exofs_dirinfo_t *out);

/* Close a directory opened with exofs_opendir(). Safe on an already-closed
 * one; there is nothing to release, so this only marks it unusable. */
int exofs_closedir(exofs_dir_t *d);

/* ---- Limits -------------------------------------------------------------
 *
 * EXOFS_MIN_SECTORS: superblock + one FAT sector + one data block, plus
 * enough slack that a volume this small is obviously a mistake rather than
 * a degenerate success.
 *
 * EXOFS_MAX_BLOCKS: the FAT is read into memory whole, at 4 bytes per block,
 * so this is really a cap on LibOS heap spent on the FAT — 262144 blocks is
 * a 128 MiB volume and a 1 MiB FAT. That is comfortable for SCRUM-190's
 * ~28 MB IWAD with room to spare, and 1 MiB is already a noticeable share of
 * a 256 MiB QEMU VM. Raising it is a one-line change here; paying for it in
 * resident memory is the part to think about first.
 */
#define EXOFS_MIN_SECTORS   8u
#define EXOFS_MAX_BLOCKS    262144u

#endif /* EXOFS_H */
