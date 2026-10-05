#include "syscall_disk.h"
#include "syscall.h"
#include "exo_syscall.h"
#include "ata.h"
#include "disk_binding.h"

#include <stddef.h>
#include <stdint.h>

/* 28-bit LBA addressing only (src/ata.h) -- the highest valid sector. */
#define ATA_MAX_LBA 0x0FFFFFFFu

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

/* First half of the shared validation: pure argument shape, no buffer, no
 * hardware, no binding. `count`/`lba` are taken as the full 64-bit values
 * the dispatcher hands every handler -- not the narrower uint32_t the
 * exo_disk_read/exo_disk_write C stubs pass -- because a caller that drives
 * the raw syscall ABI directly could put an out-of-range value in the high
 * bits, and truncating before this check would silently drop exactly the
 * bits that made it invalid.
 *
 * count == 0's "always succeeds and touches nothing" contract
 * (docs/syscall_spec.md #24/#25) runs ahead of every other rejection below
 * it in both handlers -- ownership included -- matching
 * exo_fb_acquire's own rule of validating the caller's arguments before
 * touching the resource (src/syscall_fb.c: "checked before anything is
 * allocated"). */
static int64_t validate_disk_shape(uint64_t lba, uint64_t count)
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

    return 1; /* shape validated, proceed */
}

/* Final check: the buffer, once shape, device presence and (SCRUM-188)
 * ownership have already passed. `writable` is 1 for a read (the kernel
 * writes through buf) and 0 for a write (the kernel only reads it) -- same
 * convention exo_user_range_mapped() itself uses. */
static int64_t validate_disk_buf(uint64_t buf, uint64_t count, int writable)
{
    uint64_t len = count * 512u;

    if (!exo_range_in_user_window(buf, len) ||
        !exo_user_range_mapped(buf, len, writable))
        return -EXO_EFAULT;

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

/* SCRUM-188: shared ownership gate for both handlers, run after shape and
 * device-presence checks and before the buffer check -- "no such device" is
 * more fundamental than "you don't own it" (a headless machine answers
 * -EXO_ENODEV, not -EXO_EBUSY, the same precedence exo_disk_acquire uses),
 * and an unheld or someone-else's binding is not public property, same rule
 * fb_binding_check_map() enforces for the framebuffer -- a caller with no
 * claim on the disk learns nothing about whether its buffer pointer happens
 * to be valid. */
static int owns_disk(void)
{
    return disk_binding_owner() == syscall_current_context();
}

/* #24 -- read `count` sectors starting at `lba` into `buf`. */
static int64_t sys_disk_read(uint64_t lba, uint64_t buf, uint64_t count,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    int64_t v = validate_disk_shape(lba, count);
    if (v <= 0)
        return v;

    if (!disk_binding_present())
        return -EXO_ENODEV;

    if (!owns_disk())
        return -EXO_EBUSY;

    v = validate_disk_buf(buf, count, 1);
    if (v <= 0)
        return v;

    return disk_transfer((uint32_t)lba, buf, (uint32_t)count,
                         ata_read_sector);
}

/* #25 -- write `count` sectors starting at `lba` from `buf`. */
static int64_t sys_disk_write(uint64_t lba, uint64_t buf, uint64_t count,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;

    int64_t v = validate_disk_shape(lba, count);
    if (v <= 0)
        return v;

    if (!disk_binding_present())
        return -EXO_ENODEV;

    if (!owns_disk())
        return -EXO_EBUSY;

    v = validate_disk_buf(buf, count, 0);
    if (v <= 0)
        return v;

    return disk_transfer((uint32_t)lba, buf, (uint32_t)count,
                         ata_write_sector_adapter);
}

/* #26 -- bind the disk to the caller. */
static int64_t sys_disk_acquire(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    switch (disk_binding_acquire(syscall_current_context())) {
    case DISK_BIND_OK:     return 0;
    case DISK_BIND_EBUSY:  return -EXO_EBUSY;
    default:                return -EXO_ENODEV;
    }
}

/* #27 -- give the binding back (SCRUM-189).
 *
 * No failure mode: disk_binding_release() is already a no-op for a context
 * that does not hold the disk, so there is nothing to report that the caller
 * could act on. That deliberately mirrors #26 treating a re-acquire by the
 * current owner as success -- a LibOS unwinding an error path should be able
 * to release unconditionally without first working out whether it ever
 * acquired.
 *
 * Unlike #24/#25 this does NOT check disk_binding_present() first. A caller
 * on a headless machine cannot be holding a binding, so the release is a
 * no-op either way, and answering -EXO_ENODEV would make the unconditional
 * cleanup above impossible for the one caller that needs it most. Nothing
 * here touches the ATA bus, so there is no hardware to be absent. */
static int64_t sys_disk_release(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    disk_binding_release(syscall_current_context());

    return 0;
}

void syscall_disk_init(int drive_present)
{
    disk_binding_init(drive_present);
    exo_syscall_register(EXO_SYS_DISK_READ, sys_disk_read);
    exo_syscall_register(EXO_SYS_DISK_WRITE, sys_disk_write);
    exo_syscall_register(EXO_SYS_DISK_ACQUIRE, sys_disk_acquire);
    exo_syscall_register(EXO_SYS_DISK_RELEASE, sys_disk_release);
}
