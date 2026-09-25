/*
 * exofs_blockdev.c — ExoFS sector I/O over the disk syscalls (SCRUM-189).
 * See exofs_blockdev.h for the design, and in particular for why this file
 * carries ExoFS's only #ifdef EXO_KERNEL.
 */

#include "exofs_blockdev.h"
#include "exofs_layout.h"

#include "exo_syscall.h"
#include "exo_errno.h"
#include "syscall_disk.h"   /* EXO_DISK_MAX_SECTORS */

#ifdef EXO_KERNEL
#include "syscall.h"
#endif

/* ---- The seam -----------------------------------------------------------
 *
 * Three one-line shims, and the only place the two builds differ. Written as
 * static functions rather than macros so both sides typecheck identically
 * and a mistake in the kernel-side argument order is a compile error in the
 * ring-3 build too.
 */

#ifdef EXO_KERNEL

static int64_t bdev_acquire_raw(void)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_ACQUIRE, 0, 0, 0, 0, 0, 0);
}

static int64_t bdev_release_raw(void)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_RELEASE, 0, 0, 0, 0, 0, 0);
}

static int64_t bdev_read_raw(uint32_t lba, void *buf, uint32_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_READ, (uint64_t)lba,
                                (uint64_t)(uintptr_t)buf, (uint64_t)count,
                                0, 0, 0);
}

static int64_t bdev_write_raw(uint32_t lba, const void *buf, uint32_t count)
{
    return exo_syscall_dispatch(EXO_SYS_DISK_WRITE, (uint64_t)lba,
                                (uint64_t)(uintptr_t)buf, (uint64_t)count,
                                0, 0, 0);
}

static int64_t bdev_ticks_raw(void)
{
    return exo_syscall_dispatch(EXO_SYS_GET_TICKS, 0, 0, 0, 0, 0, 0);
}

#else /* !EXO_KERNEL — the real LibOS build */

static int64_t bdev_acquire_raw(void)
{
    return exo_disk_acquire();
}

static int64_t bdev_release_raw(void)
{
    return exo_disk_release();
}

static int64_t bdev_read_raw(uint32_t lba, void *buf, uint32_t count)
{
    return exo_disk_read(lba, buf, count);
}

static int64_t bdev_write_raw(uint32_t lba, const void *buf, uint32_t count)
{
    return exo_disk_write(lba, buf, count);
}

static int64_t bdev_ticks_raw(void)
{
    return exo_get_ticks();
}

#endif /* EXO_KERNEL */

uint32_t exofs_bdev_ticks(void)
{
    int64_t t = bdev_ticks_raw();
    return (t < 0) ? 0u : (uint32_t)t;
}

/* ---- Chunked transfer ---------------------------------------------------
 *
 * exo_disk_read/exo_disk_write cap `count` at EXO_DISK_MAX_SECTORS and
 * answer -EXO_EINVAL above it (src/syscall_disk.h explains why the cap
 * exists: the whole call runs with interrupts off). Splitting here rather
 * than at every call site means the FAT writeback path can hand over a
 * whole 64 KiB FAT without knowing the number.
 *
 * read and write differ only in which raw shim they call and in buf's
 * constness, so the loop is written once over a byte cursor and a function
 * pointer would be the only other way to share it — this is the cheaper of
 * the two, given `const`.
 */

#define CHUNK EXO_DISK_MAX_SECTORS

int exofs_bdev_read(uint32_t lba, void *buf, uint32_t count)
{
    uint8_t *p = (uint8_t *)buf;

    /* count == 0 before the NULL check, deliberately and in that order: the
     * syscall itself treats a zero count as an unconditional no-op checked
     * "before everything else" (docs/syscall_spec.md §3.2 #24), so a buffer
     * that is never dereferenced is never examined either. Checking NULL
     * first would make this wrapper stricter than the call it wraps, which
     * is exactly the kind of divergence the seam exists to avoid. */
    if (count == 0) return 0;
    if (buf == NULL) return -EXO_EFAULT;

    while (count > 0) {
        uint32_t n = (count > CHUNK) ? CHUNK : count;
        int64_t  r = bdev_read_raw(lba, p, n);

        if (r < 0) return (int)r;

        lba   += n;
        p     += (size_t)n * EXOFS_BLOCK_SIZE;
        count -= n;
    }
    return 0;
}

int exofs_bdev_write(uint32_t lba, const void *buf, uint32_t count)
{
    const uint8_t *p = (const uint8_t *)buf;

    /* Same ordering as exofs_bdev_read() above, for the same reason. */
    if (count == 0) return 0;
    if (buf == NULL) return -EXO_EFAULT;

    while (count > 0) {
        uint32_t n = (count > CHUNK) ? CHUNK : count;
        int64_t  r = bdev_write_raw(lba, p, n);

        if (r < 0) return (int)r;

        lba   += n;
        p     += (size_t)n * EXOFS_BLOCK_SIZE;
        count -= n;
    }
    return 0;
}

int exofs_bdev_acquire(void)
{
    int64_t r = bdev_acquire_raw();
    return (r < 0) ? (int)r : 0;
}

void exofs_bdev_release(void)
{
    (void)bdev_release_raw();
}
