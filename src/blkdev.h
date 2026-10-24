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
 * A device index is the registration order. ata_register_blkdevs() runs first
 * and registers the primary master first, so device 0 is the system disk and
 * every fixed LBA layout constant (LIBOS_WAD_DISK_BASE_LBA, SHELL_FS_BASE_LBA,
 * EXO_VOLUME_REGION_BASE_LBA) keeps meaning what it meant when there was only
 * one disk.
 *
 * Like src/ata.c this has no notion of a caller or an owner: who may touch
 * which sectors of which device is src/disk_binding.c's business.
 */

/* Most devices the table holds: the four IDE positions. */
#define BLKDEV_MAX 4

/* Status codes from the read/write ops and blkdev_register(). The ops'
 * codes are mapped to -EXO_E* by src/syscall_disk.c; a driver translates
 * its own errors into these. */
#define BLKDEV_OK      0
#define BLKDEV_EIO   (-1)   /* the device faulted or timed out              */
#define BLKDEV_ENODEV (-2)  /* the device stopped answering                 */
#define BLKDEV_EFULL (-3)   /* blkdev_register(): table full                */

typedef struct blkdev {
    uint32_t sectors;   /* total addressable 512-byte sectors             */
    /* Transfer `count` sectors at `lba`, `count` in [1, 128]
     * (EXO_DISK_MAX_SECTORS). Bounds against `sectors` are the caller's
     * job; the ops only move data. */
    int (*read)(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count);
    int (*write)(void *ctx, uint32_t lba, const uint8_t *buf, uint32_t count);
    void *ctx;          /* driver's own handle, passed back to the ops     */
} blkdev_t;

/* Add a device. The struct is copied. Returns its index (>= 0), or
 * BLKDEV_EFULL. */
int blkdev_register(const blkdev_t *dev);

/* The device at `index`, or NULL if there is none. */
const blkdev_t *blkdev_get(uint32_t index);

/* How many devices are registered; valid indices are [0, blkdev_count()). */
uint32_t blkdev_count(void);

/* Drop every device at index >= `n` (no-op if there are not that many).
 * For tests that register fake devices and must leave the real ones as they
 * found them: save blkdev_count(), register, then truncate back to it. */
void blkdev_truncate(uint32_t n);
