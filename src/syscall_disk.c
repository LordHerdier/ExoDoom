#include "syscall_disk.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ata.h"

#include <stddef.h>
#include <stdint.h>

/* 28-bit LBA addressing only (src/ata.h) -- the highest valid sector. */
#define ATA_MAX_LBA 0x0FFFFFFFu

/* Set by syscall_disk_init() from ata_init()'s own result, so every call
 * answers -EXO_ENODEV without re-probing the bus. */
static int g_disk_present = 0;

static int64_t ata_err_to_exo(int rc)
{
    switch (rc) {
    case ATA_ENODEV:  return -EXO_ENODEV;
    case ATA_ETIMEOUT: /* fall through */
    case ATA_EIO:      return -EXO_EIO;
    default:            return -EXO_EIO;
    }
}

/* ata_write_sector takes `const uint8_t *`; disk_transfer()'s callback type
 * is `uint8_t *` (the shape ata_read_sector already has) so one loop can
 * drive either direction. Adapting here, rather than casting the function
 * pointer itself, keeps every call through it correctly typed -- casting a
 * function pointer to a signature whose parameter type differs is undefined
 * behaviour even when the underlying calling convention matches. */
static int ata_write_sector_adapter(uint32_t lba, uint8_t *buf)
{
    return ata_write_sector(lba, buf);
}

/* Shared bounds/validation for both handlers. `count`/`lba` are taken as the
 * full 64-bit values the dispatcher hands every handler -- not the narrower
 * uint32_t the exo_disk_read/exo_disk_write C stubs pass -- because a caller
 * that drives the raw syscall ABI directly could put an out-of-range value
 * in the high bits, and truncating before this check would silently drop
 * exactly the bits that made it invalid. `writable` is 1 for a read (the
 * kernel writes through buf) and 0 for a write (the kernel only reads it) --
 * same convention exo_user_range_mapped() itself uses.
 *
 * Checked in this order deliberately: every argument-shape rejection
 * (count == 0, oversized count, LBA range, bad buffer) happens before the
 * drive-presence check, not after -- so `count == 0`'s "always succeeds and
 * touches nothing" contract (docs/syscall_spec.md #27/#28) and every other
 * bounds rejection hold regardless of whether a drive is attached, matching
 * exo_fb_acquire's own rule of validating the caller's arguments before
 * touching the resource (src/syscall_fb.c: "checked before anything is
 * allocated"). -EXO_ENODEV is reserved for the one thing that genuinely
 * needs the hardware: an argument-valid request with nothing to serve it. */
static int64_t validate_disk_args(uint64_t lba, uint64_t buf, uint64_t count,
                                  int writable)
{
    if (count == 0)
        return 0; /* handled specially by callers: no-op success */

    if (count > EXO_DISK_MAX_SECTORS)
        return -EXO_EINVAL;

    /* Reject an out-of-range lba on its own first: count is already known
     * small (<= EXO_DISK_MAX_SECTORS), so once lba itself is within 28-bit
     * range the addition below cannot wrap -- but a raw-syscall caller could
     * still hand in an lba near UINT64_MAX, which this catches before that
     * addition ever runs. */
    if (lba > ATA_MAX_LBA || lba + count - 1 > ATA_MAX_LBA)
        return -EXO_EINVAL;

    uint64_t len = count * 512u;
    if (!exo_range_in_user_window(buf, len) ||
        !exo_user_range_mapped(buf, len, writable))
        return -EXO_EFAULT;

    if (!g_disk_present)
        return -EXO_ENODEV;

    return 1; /* validated, proceed */
}

/* Shared sector loop for both directions -- `xfer` is ata_read_sector for a
 * read, ata_write_sector_adapter for a write. `lba`/`count` are already
 * known to fit their 28-bit/EXO_DISK_MAX_SECTORS ranges by the time this
 * runs (validate_disk_args checked the untruncated 64-bit values). */
static int64_t disk_transfer(uint32_t lba, uint64_t buf, uint32_t count,
                             int (*xfer)(uint32_t, uint8_t *))
{
    uint8_t *p = (uint8_t *)(uintptr_t)buf;

    for (uint32_t i = 0; i < count; i++) {
        int rc = xfer(lba + i, p + (uint64_t)i * 512u);
        if (rc != ATA_OK)
            return ata_err_to_exo(rc);
    }

    return (int64_t)count;
}

/* #27 -- read `count` sectors starting at `lba` into `buf`. */
static int64_t sys_disk_read(uint64_t lba, uint64_t buf, uint64_t count,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    int64_t v = validate_disk_args(lba, buf, count, 1);
    if (v <= 0)
        return v;

    return disk_transfer((uint32_t)lba, buf, (uint32_t)count,
                         ata_read_sector);
}

/* #28 -- write `count` sectors starting at `lba` from `buf`. */
static int64_t sys_disk_write(uint64_t lba, uint64_t buf, uint64_t count,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    int64_t v = validate_disk_args(lba, buf, count, 0);
    if (v <= 0)
        return v;

    return disk_transfer((uint32_t)lba, buf, (uint32_t)count,
                         ata_write_sector_adapter);
}

void syscall_disk_init(int drive_present)
{
    g_disk_present = drive_present;
    exo_syscall_register(EXO_SYS_DISK_READ, sys_disk_read);
    exo_syscall_register(EXO_SYS_DISK_WRITE, sys_disk_write);
}
