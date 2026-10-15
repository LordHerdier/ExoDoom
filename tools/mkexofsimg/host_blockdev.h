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
 */

int  host_blockdev_open(const char *path, uint32_t total_sectors);
void host_blockdev_close(void);

#endif /* HOST_BLOCKDEV_H */
