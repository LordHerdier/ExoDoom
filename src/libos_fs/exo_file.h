#ifndef EXO_FILE_H
#define EXO_FILE_H

#include <stdint.h>

#include "exofs.h"

/*
 * exo_file.h — descriptor-based file API over ExoFS (SCRUM-44).
 *
 * This is the layer a libc-style caller (the FILE* shim, SCRUM-42) sits on:
 * small integer descriptors instead of exofs.h's caller-allocated
 * exofs_file_t handles. It is LibOS code, not a syscall surface. The kernel
 * knows sectors (exo_disk_read/exo_disk_write) and nothing above them;
 * dispatcher numbers #9-#16 were once reserved for kernel file syscalls and
 * stay unbound for good (docs/syscall_spec.md §4). The analogy is malloc
 * over exo_page_alloc: policy in the library, mechanism in the kernel.
 *
 * ERRORS. Same convention as exofs.h: a non-negative result on success,
 * a negative EXO_E* code from src/exo_errno.h otherwise. ExoFS errors pass
 * through unchanged. This layer adds only two of its own:
 *   -EXO_EBADF   the descriptor is negative, out of range, or not open;
 *   -EXO_EMFILE  every descriptor in this program's table is in use.
 * -EXO_ENFILE, by contrast, comes from ExoFS's own open-entry table
 * (EXOFS_MAX_OPEN_HANDLES) and is passed through as-is.
 *
 * NO MOUNT POLICY. The path-taking calls (open, stat) resolve against one
 * *default volume*, which the caller names with exo_file_set_volume() after
 * exofs_mount(). Descriptors remember the volume they were opened on, so
 * changing the default later does not move an open file. With no default set
 * (or a default that has been unmounted), open and stat answer ExoFS's
 * -EXO_EINVAL. Choosing, formatting and mounting a volume is the caller's
 * decision, not this library's (docs/filesystem.md §8). The default is not a
 * mount table: paths are not routed between volumes here.
 *
 * DURABILITY. A file's size reaches the disk on close, not on every write
 * (exofs.h's note on exofs_file_t). **Close every descriptor — or call
 * exo_file_close_all() — before exofs_unmount()**, or the data blocks
 * survive and the size does not, and the file silently reads back short.
 *
 * ONE TABLE PER PROGRAM. The table is a static inside this program's copy of
 * src/libos_fs/, the same way ExoFS's volume pool is. There is no
 * threading and no locking, for the reasons exofs.h gives.
 */

/* Set the volume exo_file_open() / exo_file_stat() resolve paths against.
 * NULL clears it. Clear it (or replace it) when unmounting that volume. */
void exo_file_set_volume(exofs_vol_t *v);

/* The default volume, or NULL if none is set or it has been unmounted. */
exofs_vol_t *exo_file_volume(void);

/* Open flags. Aliases of ExoFS's, so they pass straight through; combine
 * them. One of EXO_FILE_O_READ / EXO_FILE_O_WRITE must be present. */
#define EXO_FILE_O_READ    EXOFS_O_READ
#define EXO_FILE_O_WRITE   EXOFS_O_WRITE
#define EXO_FILE_O_CREATE  EXOFS_O_CREATE   /* create if absent              */
#define EXO_FILE_O_TRUNC   EXOFS_O_TRUNC    /* discard existing contents     */
#define EXO_FILE_O_APPEND  EXOFS_O_APPEND   /* every write goes to the end   */

/* Whence values, in stdio's SEEK_SET/CUR/END order. */
#define EXO_FILE_SEEK_SET  EXOFS_SEEK_SET
#define EXO_FILE_SEEK_CUR  EXOFS_SEEK_CUR
#define EXO_FILE_SEEK_END  EXOFS_SEEK_END

/*
 * Descriptors per program. Matches EXOFS_MAX_OPEN_HANDLES: ExoFS cannot
 * hold more entries open than that at once, so a bigger table would only
 * move the failure from -EXO_EMFILE to -EXO_ENFILE.
 */
#define EXO_FILE_MAX_FDS   8

/*
 * Open `path` (absolute, docs/filesystem.md §7) with `flags`. Returns the
 * lowest free descriptor (>= 0), -EXO_EMFILE if none is free, or any error
 * exofs_open() reports — -EXO_ENOENT, -EXO_EISDIR, -EXO_EINVAL, -EXO_EBUSY,
 * -EXO_ENFILE, -EXO_ENOSPC or a device error. A failed open consumes no
 * descriptor.
 */
int exo_file_open(const char *path, uint32_t flags);

/*
 * Flush and close `fd`. The descriptor is free afterwards even if the flush
 * failed — there is nothing more a caller could do with it — and that flush
 * error is what gets returned. -EXO_EBADF for a descriptor that is not open,
 * including a second close of the same one.
 */
int exo_file_close(int fd);

/* Read up to `n` bytes at the current position. Returns the count read
 * (0 at end of file), -EXO_EBADF, -EXO_EACCES for a descriptor opened
 * without EXO_FILE_O_READ, or a device error. */
int64_t exo_file_read(int fd, void *buf, uint32_t n);

/* Write `n` bytes at the current position (at end of file under
 * EXO_FILE_O_APPEND), extending the file as needed. Returns the count
 * written, -EXO_EBADF, -EXO_EACCES for a descriptor opened without
 * EXO_FILE_O_WRITE, -EXO_ENOSPC, or a device error. */
int64_t exo_file_write(int fd, const void *buf, uint32_t n);

/* Move the position; `whence` is one of EXO_FILE_SEEK_*. Returns the new
 * absolute position — so exo_file_seek(fd, 0, EXO_FILE_SEEK_CUR) is ftell —
 * or -EXO_EBADF, or -EXO_EINVAL for an unknown whence or a position before
 * the start. Seeking past the end is allowed (exofs.h, holes). */
int64_t exo_file_seek(int fd, int64_t off, uint32_t whence);

/* Write the size in bytes of `path` to *size_out (0 for a directory).
 * Returns 0, -EXO_EINVAL for a NULL size_out, or any error exofs_stat()
 * reports — -EXO_ENOENT for a missing path. Sizes of files still open for
 * writing are as of their last close or flush, not their last write. */
int exo_file_stat(const char *path, uint32_t *size_out);

/* Close every open descriptor. Returns 0, or the first close error seen;
 * every descriptor is closed either way. Call before exofs_unmount(). */
int exo_file_close_all(void);

#endif /* EXO_FILE_H */
