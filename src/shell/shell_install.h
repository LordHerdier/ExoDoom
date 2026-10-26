/*
 * shell_install.h — the copy behind the shell's `install` command
 * (SCRUM-233).
 *
 * `install <name>` copies one file from the source volume (the install
 * stick, block device 1) onto the system volume (LIBOS_WAD_DISK_BASE_LBA on
 * device 0), after which `programs` lists it and `run` launches it. ExoFS
 * mounts one volume at a time per program (docs/filesystem.md §8), so the
 * copy alternates: mount the source, read a chunk, unmount, mount the
 * destination, write it, unmount. Nothing here touches the framebuffer
 * console or the disk binding, so the same TU links into the ring-3 shell
 * blob and, under TESTING=1, into the kernel for
 * tests/kernel/test_shell_install_k.c (docker/scripts/build.sh) -- the same
 * arrangement as shell_help.c.
 */
#ifndef EXODOOM_SHELL_INSTALL_H
#define EXODOOM_SHELL_INSTALL_H

#include <stddef.h>
#include <stdint.h>

/* An ExoFS volume: which block device, and the LBA its superblock is at. */
typedef struct {
    uint32_t dev;
    uint32_t base;
} shell_vol_t;

/*
 * Turn the user's `install` argument into the path on both volumes: a
 * leading '/' is added when missing (names are relative to the volume root,
 * like `run`'s), and a name whose last component has no '.' gets ".exo",
 * so `install doom` means /doom.exo while `install freedoom2.wad` is taken
 * as written. Returns 0, or -EXO_EINVAL for an empty name or one that does
 * not fit in `out` -- a truncated name is a different file.
 */
int shell_install_resolve(const char *name, char *out, size_t out_len);

/*
 * Copy `path` from `src` to `dst` through `buf` (`chunk` bytes, from
 * libos_heap_alloc() -- never malloc(), CLAUDE.md), replacing any existing
 * file of that name on `dst`. The caller holds the disk binding for both
 * volumes and keeps it; this only mounts and unmounts, and returns with
 * nothing mounted.
 *
 * Returns 0 and the byte count in `*copied`, -EXO_ENOENT if `src` has no
 * such file (dst untouched), -EXO_EISDIR for a directory, -EXO_ENOSPC if
 * `dst` filled, or the first mount/read/write error. On a failure after the
 * destination file was created it is unlinked again, so a failed install
 * never leaves a truncated package for `run` to trip over.
 */
int shell_install_copy(const shell_vol_t *src, const shell_vol_t *dst,
                       const char *path, uint8_t *buf, uint32_t chunk,
                       uint32_t *copied);

/*
 * shell_install_copy() with a different path on each side (SCRUM-266, for
 * `cp`/`mv` across mounted volumes): copy `spath` on `src` to `dpath` on
 * `dst`. Same contract and the same two rules -- the source is stat'ed
 * before the destination is touched, and a failed copy unlinks the
 * destination it created. `src` and `dst` must be held through different
 * disk bindings the caller already owns; this only mounts and unmounts.
 */
int shell_copy_between(const shell_vol_t *src, const char *spath,
                       const shell_vol_t *dst, const char *dpath,
                       uint8_t *buf, uint32_t chunk, uint32_t *copied);

/* 1 if a resolved path names a pattern (contains '*') rather than a file. */
int shell_install_is_glob(const char *path);

/*
 * Match one file name against a pattern in which '*' stands for any run of
 * characters, including none, and every other character only for itself
 * (case-sensitive, like ExoFS names). `install *` resolves to "*.exo"
 * (shell_install_resolve()'s no-extension rule), which is what makes it
 * mean "every package".
 */
int shell_install_glob_match(const char *pattern, const char *name);

/*
 * List the files at the root of `src` whose names match `pattern` (a
 * resolved path: "/" then a name pattern, with no further '/' -- wildcards
 * only reach the top of the install disk). Each match is stored as an
 * absolute path, "/<name>", at `names + i * stride`, in directory order.
 * Directories never match. The caller holds the binding; this mounts and
 * unmounts.
 *
 * Returns 0 with *count matches, -EXO_EINVAL for a pattern with a '/' after
 * the leading one, -EXO_ENOSPC if more than `max` files match (nothing
 * should be installed from a list that was cut short), or a mount or
 * directory error.
 */
int shell_install_matches(const shell_vol_t *src, const char *pattern,
                          char *names, uint32_t stride, uint32_t max,
                          uint32_t *count);

#endif /* EXODOOM_SHELL_INSTALL_H */
