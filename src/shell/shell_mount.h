/*
 * shell_mount.h — the shell's mount table and path router (SCRUM-266).
 *
 * `mount disk1:vol1 /mnt/x` makes the volume on the right of the colon
 * reachable under /mnt/x. ExoFS can mount only one volume at a time
 * (docs/filesystem.md §8), so this is not a filesystem-level mount: it is a
 * table of (directory, volume) pairs, and the shell mounts whichever volume
 * a path routes to for the length of one command. Everything here is pure
 * string and table logic -- no framebuffer console, no syscalls, no libc --
 * so the same TU links into the ring-3 shell blob and, under TESTING=1, into
 * the kernel for tests/kernel/test_shell_mount_k.c (docker/scripts/build.sh),
 * the arrangement shell_help.c and shell_install.c already use.
 *
 * PATHS. Everything taking a path here takes an absolute one and, except for
 * shell_mount_normalize() itself, an already-normalized one. Normalizing
 * first matters beyond tidiness: ExoFS resolves ".." through the directory's
 * own entries, so "/mnt/x/.." would be handed to the mounted volume, whose
 * root's ".." is itself, instead of leading back to the volume /mnt lives on.
 */
#ifndef EXODOOM_SHELL_MOUNT_H
#define EXODOOM_SHELL_MOUNT_H

#include <stddef.h>
#include <stdint.h>

/* At most this many mount points at once. */
#define SHELL_MOUNT_MAX      8u

/* Longest mount-point path, NUL included. Matches the shell's resolved-path
 * buffer (SHELL_FS_RESOLVED_BUF_LEN in shell_main.c). */
#define SHELL_MOUNT_PATH_LEN 128u

/*
 * Where an ExoFS volume is and which disk sectors the shell must hold to
 * reach it. The extent is separate from the base because the binding is one
 * extent per owner per device (docs/syscall_spec.md §3.5a): the shell volume
 * lives inside [0, LIBOS_WAD_DISK_BASE_LBA) on device 0, a program slot has
 * a slot-sized extent of its own, and a second disk is taken whole
 * (extent_base == 0, extent_count == 0, as exo_disk_acquire() reads it).
 */
typedef struct {
    uint32_t dev;
    uint32_t base;          /* LBA of the superblock                    */
    uint32_t extent_base;   /* exo_disk_acquire() arguments             */
    uint32_t extent_count;
    uint32_t volno;         /* the N of diskD:volN it was named by, for
                             * listings; routing never looks at it       */
} shell_volref_t;

typedef struct {
    int used;
    char point[SHELL_MOUNT_PATH_LEN];
    shell_volref_t vol;
} shell_mount_entry_t;

typedef struct {
    shell_mount_entry_t e[SHELL_MOUNT_MAX];
} shell_mount_table_t;

/*
 * Collapse "//", "." and ".." out of the absolute path `in` into `out`
 * ("/a/./b//../c/" -> "/a/c"). ".." never climbs above "/". The result has
 * no trailing '/' except for the root itself.
 *
 * Returns 0, or -EXO_EINVAL if `in` is not absolute or the result does not
 * fit in `out_len` bytes (a truncated path names a different directory).
 * `in` and `out` must not overlap.
 */
int shell_mount_normalize(const char *in, char *out, size_t out_len);

/* 1 if the normalized `path` is `dir` or lies beneath it, comparing whole
 * components ("/mnt/xy" is not under "/mnt/x"). "/" contains everything. */
int shell_mount_path_within(const char *path, const char *dir);

/* Empty the table. */
void shell_mount_init(shell_mount_table_t *t);

/*
 * Record `vol` as mounted at `point` (normalized, absolute, not "/").
 * Returns 0, -EXO_EINVAL (not such a path, or "/": the root is always the
 * shell's own volume), -EXO_EBUSY (something is already mounted exactly
 * there), or -EXO_ENOSPC (table full). Whether the directory exists and is
 * empty is the caller's check; it needs the filesystem.
 */
int shell_mount_add(shell_mount_table_t *t, const char *point,
                    const shell_volref_t *vol);

/*
 * Drop the mount at `point`. `cwd` is the shell's working directory
 * (normalized): a mount the shell is standing in cannot go.
 * Returns 0, -EXO_EINVAL (nothing is mounted exactly there), or
 * -EXO_EBUSY (`cwd` is at or below it, or another mount lies beneath it).
 */
int shell_mount_remove(shell_mount_table_t *t, const char *point,
                       const char *cwd);

/* The index of the mount whose point is exactly `path`, or -1. */
int shell_mount_find(const shell_mount_table_t *t, const char *path);

/* 1 if some mount point lies strictly beneath `path`. */
int shell_mount_has_below(const shell_mount_table_t *t, const char *path);

/*
 * Which volume holds the normalized absolute `path`, and the path within it.
 *
 * The longest mount point that contains `path` wins, so a mount nested
 * inside another routes to the inner one. With no match the path is on the
 * root volume `root`. `*vol` is the volume and `inner` receives the path
 * relative to its root ("/" for the volume's root itself), at most `inner_len`
 * bytes with the NUL. `*mount_idx` is the table index that matched, or -1 for
 * the root volume; combined with inner == "/" it says "this path IS a mount
 * point", which `rm` and `mv` must refuse to touch.
 *
 * Returns 0, or -EXO_EINVAL for a path that is not absolute or an `inner`
 * that is too small.
 */
int shell_mount_route(const shell_mount_table_t *t, const shell_volref_t *root,
                      const char *path, shell_volref_t *vol, char *inner,
                      size_t inner_len, int *mount_idx);

/*
 * Parse a volume name "disk<N>:vol<M>" into the block device index `*disk`
 * (0 = the system disk) and the 1-based volume number `*vol`. Lowercase
 * only, decimal digits only, no sign, no spaces; `vol` must be at least 1
 * and neither number may exceed 9999 (far past any real disk or volume
 * count, and it keeps the arithmetic from overflowing on a long run of
 * digits). Returns 0, or -EXO_EINVAL.
 */
int shell_mount_parse_volname(const char *s, uint32_t *disk, uint32_t *vol);

/*
 * Whether one 512-byte sector looks like an ExoFS superblock: the magic, the
 * version and the block size. It is the cheap probe `disk` and `mount` use
 * to decide a candidate location holds a volume at all, without paying for a
 * mount (which reads the whole FAT). exofs_mount() still validates the
 * geometry properly, so a sector that passes here can still fail to mount.
 */
int shell_mount_sb_valid(const void *sector);

#endif /* EXODOOM_SHELL_MOUNT_H */
