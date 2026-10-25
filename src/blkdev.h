#pragma once

#include <stdint.h>

/*
 * blkdev.h — kernel block-device registry (SCRUM-244).
 *
 * Before this the storage stack could see exactly one disk: src/ata.c spoke
 * to the primary master and nothing else, and exo_disk_* (#24–#27, #31) had
 * no way to name a device. This table is the device dimension. A driver that
 * finds a disk registers it here -- src/ata.c for every IDE position that
 * answers IDENTIFY (ata_register_blkdevs()), later the USB mass-storage
 * driver (SCRUM-247) -- and src/syscall_disk.c reaches the hardware only
 * through blkdev_get(dev), never by calling a driver directly.
 *
 * INDEX 0 IS RESERVED FOR THE SYSTEM DISK. Every fixed LBA layout constant
 * (LIBOS_WAD_DISK_BASE_LBA, SHELL_FS_BASE_LBA, EXO_VOLUME_REGION_BASE_LBA)
 * and the shell's first-boot "format the system volume?" offer assume device
 * 0 is the system disk, so it is a slot only blkdev_register_system() fills
 * -- src/ata.c does, with the primary master. Every other disk goes in the
 * first free slot from 1 up. With no system disk, slot 0 stays EMPTY rather
 * than being taken by whatever disk was found next: a lone install stick
 * must never be mistaken for the system volume and offered a format.
 * Indices can therefore have a hole at 0, which is why exo_diskstat (#31)
 * callers enumerate every index below EXO_DISK_MAX_DEVICES rather than
 * stopping at the first absent one.
 *
 * This is also the single source of truth for which devices exist:
 * src/disk_binding.c's disk_binding_present() asks blkdev_get() rather than
 * keeping a count of its own.
 *
 * Like src/ata.c this has no notion of a caller or an owner: who may touch
 * which sectors of which device is src/disk_binding.c's business.
 */

/* Most devices the table holds: the four IDE positions. */
#define BLKDEV_MAX 4

/* The slot reserved for the system disk. */
#define BLKDEV_SYSTEM 0u

/* Status codes from the read/write ops and the register calls. The ops'
 * codes are mapped to -EXO_E* by src/syscall_disk.c; a driver translates
 * its own errors into these. */
#define BLKDEV_OK      0
#define BLKDEV_EIO   (-1)   /* the device faulted or timed out              */
#define BLKDEV_ENODEV (-2)  /* the device stopped answering                 */
#define BLKDEV_EFULL (-3)   /* blkdev_register(): no free slot              */
#define BLKDEV_EBUSY (-4)   /* blkdev_register_system(): slot 0 is taken    */

typedef struct blkdev {
    uint32_t sectors;   /* total addressable 512-byte sectors             */
    /* Transfer `count` sectors at `lba`, `count` in [1, 128]
     * (EXO_DISK_MAX_SECTORS). Bounds against `sectors` are the caller's
     * job; the ops only move data. */
    int (*read)(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count);
    int (*write)(void *ctx, uint32_t lba, const uint8_t *buf, uint32_t count);
    void *ctx;          /* driver's own handle, passed back to the ops     */
} blkdev_t;

/* Put the system disk in slot BLKDEV_SYSTEM. The struct is copied. Returns
 * BLKDEV_SYSTEM, or BLKDEV_EBUSY if the slot is already filled. */
int blkdev_register_system(const blkdev_t *dev);

/* Add any other disk, in the first free slot from 1 up. The struct is
 * copied. Returns its index (>= 1), or BLKDEV_EFULL. */
int blkdev_register(const blkdev_t *dev);

/* The device at `index`, or NULL if that slot is empty or out of range. */
const blkdev_t *blkdev_get(uint32_t index);

/* How many slots are filled. Not a bound on valid indices: slot 0 may be
 * empty while later ones are filled. */
uint32_t blkdev_count(void);

/* ---- Test support --------------------------------------------------------
 *
 * The KUnit suites register fake devices, or empty the table to model a
 * machine with no disk, and must leave the boot-time registrations exactly as
 * kernel_main made them for every suite after them: save first, restore
 * after. Nothing on the boot path calls these. */

typedef struct {
    blkdev_t devs[BLKDEV_MAX];
    uint8_t  used[BLKDEV_MAX];
} blkdev_snapshot_t;

void blkdev_save(blkdev_snapshot_t *out);
void blkdev_restore(const blkdev_snapshot_t *in);

/* Empty slot `index` (no-op if it already is, or is out of range). */
void blkdev_unregister(uint32_t index);
