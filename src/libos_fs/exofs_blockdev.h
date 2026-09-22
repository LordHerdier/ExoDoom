#ifndef EXOFS_BLOCKDEV_H
#define EXOFS_BLOCKDEV_H

#include <stdint.h>

/*
 * exofs_blockdev — the only part of ExoFS that knows a syscall exists
 * (SCRUM-189).
 *
 * Everything above this file works in blocks and LBAs; this file turns those
 * into exo_disk_read/exo_disk_write/exo_disk_acquire (#27/#28/#29,
 * docs/syscall_spec.md §3.2, src/syscall_disk.c). Keeping that in one place
 * is what makes the rest of the filesystem testable from ring 0 — see the
 * dual-compile note below — and it is also the seam a future ramdisk or
 * image-file backend would replace.
 *
 * DUAL COMPILE. This file has the one `#ifdef EXO_KERNEL` in ExoFS, and it
 * is the same mechanism src/libos_page_alloc.c already uses and explains at
 * length in its own top comment:
 *
 *   - built WITHOUT -DEXO_KERNEL (a ring-3 link target, the real LibOS
 *     build): the inline exo_disk_*() stubs from src/exo_syscall.h, which
 *     execute an actual `syscall` instruction.
 *   - built WITH -DEXO_KERNEL (compiled into the kernel image under
 *     TESTING=1, which is how tests/kernel/test_exofs_k.c drives it):
 *     exo_syscall_dispatch() from src/syscall.h — a plain C call into the
 *     very same handler, with no privilege transition.
 *
 * The second form is not a convenience. The stubs return via `sysretq`,
 * which unconditionally forces CPL 3 on the way out, so calling one from
 * ring-0 code silently drops the CPU to ring 3 for everything that runs
 * afterwards — libos_page_alloc.c's comment describes that failure happening
 * for real. The dispatcher call proves the same handler behaviour (binding
 * checks, bounds checks, error codes) without the hazard.
 *
 * BUFFERS MUST LIVE IN THE LibOS VA WINDOW. exo_disk_read rejects any `buf`
 * that is not entirely inside [EXO_USER_VA_BASE, EXO_USER_VA_END) and mapped
 * with -EXO_EFAULT, and it does so under EXO_KERNEL too, because it is
 * literally the same handler. So ExoFS never takes a disk buffer from
 * malloc(): under EXO_KERNEL that is kmalloc(), a kernel bump address, and
 * every transfer would fail -EXO_EFAULT. Buffers come from
 * libos_heap_alloc() (src/libos_heap.h), which sits on libos_page_alloc()
 * and hands out window addresses under both compiles. Callers of this file
 * are responsible for that; exofs_bdev_read/_write do not allocate.
 *
 * ERRORS. Negative EXO_E* (src/exo_errno.h), passed through from the
 * handler unchanged — there is no translation layer, so -EXO_EBUSY really
 * does mean "this context does not hold the disk binding" and not something
 * ExoFS invented.
 */

/*
 * Acquire the disk binding for this context (#29, SCRUM-188).
 *
 * exo_disk_read/exo_disk_write answer -EXO_EBUSY to a caller that does not
 * hold the binding, whether nobody holds it or somebody else does, so this
 * must succeed before any transfer will. Re-acquiring when this context
 * already holds it is success, not an error, so callers may call it
 * unconditionally.
 *
 * Returns 0, -EXO_EBUSY (another context holds the disk) or -EXO_ENODEV
 * (this machine has no drive).
 */
int exofs_bdev_acquire(void);

/*
 * Read/write `count` consecutive 512-byte sectors at absolute LBA `lba`.
 *
 * `buf` must be at least count * EXOFS_BLOCK_SIZE bytes and must lie in the
 * LibOS VA window (see above). Returns 0 on success, or a negative EXO_E*.
 *
 * Unlike the raw syscall these return 0 rather than `count`: a partial
 * transfer is not a thing either syscall can report (docs/syscall_spec.md
 * §3.2 #27 — a sector fault is -EXO_EIO with no byte count), so "how many"
 * carries no information a caller could act on, and collapsing it removes
 * the temptation to write a partial-transfer loop that can never run.
 *
 * `count` may exceed EXO_DISK_MAX_SECTORS (128, src/syscall_disk.h); these
 * split the transfer into capped chunks rather than rejecting it, so callers
 * can read a whole FAT in one call without restating the cap.
 */
int exofs_bdev_read(uint32_t lba, void *buf, uint32_t count);
int exofs_bdev_write(uint32_t lba, const void *buf, uint32_t count);

/*
 * Monotonic milliseconds since boot, for directory-entry timestamps
 * (exo_get_ticks, #5). Lives here rather than beside its caller for the same
 * reason the transfers do: it is a syscall, so it needs the dual-compile
 * seam, and there should be exactly one file in ExoFS that knows that.
 *
 * Not a wall clock — there is none — so this is only ever an ordering within
 * one boot. exofs_layout.h's "Time" note has the consequence: a volume
 * carried across a reboot holds timestamps from a previous boot's tick
 * count, so nothing should compare them across a mount. Returns 0 rather
 * than failing if the syscall errors; a timestamp is not worth failing an
 * otherwise good create over.
 */
uint32_t exofs_bdev_ticks(void);

#endif /* EXOFS_BLOCKDEV_H */
