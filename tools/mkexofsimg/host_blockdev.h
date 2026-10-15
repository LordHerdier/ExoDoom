#ifndef HOST_BLOCKDEV_H
#define HOST_BLOCKDEV_H

#include <stdint.h>

/*
 * host_blockdev — the host-side stand-in for src/libos_fs/exofs_blockdev.c
 * (SCRUM-190).
 *
 * exofs_blockdev.c has exactly two compile modes (EXO_KERNEL's dispatcher
 * call, or the ring-3 inline `syscall` stub) and neither exists on the
 * build host. This is a third, independent implementation of the same
 * seam -- exofs_bdev_read/_write/_acquire/_release/_ticks, declared in
 * src/libos_fs/exofs_blockdev.h -- backed by plain file I/O against a local
 * .img file instead of a syscall. Nothing in that header's contract is
 * kernel- or LibOS-specific, so this is a legitimate fourth caller of the
 * same interface, not a hack grafted onto it.
 *
 * host_blockdev_open() creates/truncates the image to exactly
 * total_sectors * 512 bytes, zero-filled -- exofs_format() assumes it is
 * laying down a blank volume, the same assumption a real blank drive
 * satisfies by being all zero out of the factory.
 *
 * host_blockdev_open_existing() is the other half, added for
 * tools/exofsfuse (SCRUM-229 follow-up): it attaches to an image that
 * already holds a volume, without destroying it. `read_only` opens the
 * file "rb" instead of "r+b", after which exofs_bdev_write() fails -- the
 * FUSE driver uses that for an image it cannot write (build/exodoom_disk.img
 * is written by a root-owned Docker build), and refuses modifying calls
 * itself rather than letting them reach a failing write.
 *
 * Returns 0, or -1 if the file cannot be opened or is not a whole number
 * of 512-byte sectors.
 */

int  host_blockdev_open(const char *path, uint32_t total_sectors);
int  host_blockdev_open_existing(const char *path, int read_only);

/* Total 512-byte sectors in the currently open image, or 0 if none is. */
uint32_t host_blockdev_sectors(void);

void host_blockdev_close(void);

#endif /* HOST_BLOCKDEV_H */
