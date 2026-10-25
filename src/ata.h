#pragma once
#include <stdint.h>

/*
 * ata — polled PIO driver for the legacy ATA buses (SCRUM-102; every
 * position since SCRUM-244).
 *
 * Ring-0 only, and deliberately syscall-free: exo_disk_read/exo_disk_write
 * (SCRUM-103) are a separate ticket that puts a bounds-checked syscall
 * boundary in front of this, plus a disk ownership/binding table (SCRUM-188)
 * mirroring fb_binding.c. This file has no notion of a caller, an owner, or
 * a filesystem -- it is the same relationship src/pic.c/pit.c have to their
 * hardware, not the relationship syscall_fb.c has to fb_binding.c.
 *
 * QEMU's `-drive if=ide` attaches to the primary bus (ports 0x1F0-0x1F7 data/
 * command, 0x3F6 device control) as the master drive -- confirmed as the
 * chosen interface on the Jira ticket (2026-09-19), rejecting SCSI/virtio-scsi
 * as unnecessary complexity for a QEMU-only demo. SCRUM-244 lifted the
 * one-drive limit: ata_init() probes all four positions (primary/secondary
 * bus x master/slave, the order QEMU's `index=0..3` numbers them), skips an
 * ATAPI CD-ROM (QEMU's `-cdrom` is the secondary master), and
 * ata_register_blkdevs() puts every disk that answered into the block-device
 * registry (src/blkdev.h), which is how the syscall layer reaches it. The
 * unsuffixed calls below still mean the primary master, so ring-0 users of
 * the system disk (and test_ata_k.c) are unchanged; every other drive is
 * reached only through the block-device ops ata_register_blkdevs()
 * registers.
 *
 * Polled, not IRQ14-driven: "PIO" here means programmed I/O with a
 * busy-polled status register, matching this ticket's minimal acceptance bar
 * (detect a drive, round-trip a sector) without adding a new IDT vector.
 * 28-bit LBA addressing only -- ample for the small FAT image SCRUM-190 will
 * attach; 48-bit LBA is not needed at this scale.
 */

#define ATA_OK        0
#define ATA_ENODEV  (-1)   /* no drive present (status read as 0x00)        */
#define ATA_ETIMEOUT (-2)  /* BSY never cleared / DRQ never set             */
#define ATA_EIO     (-3)   /* ERR bit set in the status register            */

/* Number of IDE positions probed: primary master, primary slave, secondary
 * master, secondary slave. */
#define ATA_MAX_DRIVES 4

/* Probe every position with IDENTIFY. Safe to call again (the KUnit suites
 * do): it re-probes and overwrites the per-drive state, and registers
 * nothing -- registration is ata_register_blkdevs(), once, at boot.
 *
 * The return value is the PRIMARY MASTER's: ATA_OK if a drive answered,
 * ATA_ENODEV if the bus reads back all-zero status (no drive wired), or
 * ATA_ETIMEOUT/ATA_EIO on a drive that responded but faulted. Must be called
 * once before ata_read_sector()/ata_write_sector(); safe to call from a
 * TESTING build (no drive attached to docker-run/docker-run-kernel still
 * reports ATA_ENODEV rather than halting -- see src/kernel.c's call site).
 */
int ata_init(void);

/* Read/write exactly one 512-byte sector at 28-bit LBA `lba`. `buf` must
 * point to (at least) 512 bytes. Returns ATA_OK, or a negative ATA_* status.
 * Undefined behaviour (a hardware timeout, not a crash) if called before a
 * successful ata_init(). Thin wrappers around ata_read_sectors()/
 * ata_write_sectors() below, count 1.
 */
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);

/* Read/write `count` consecutive 512-byte sectors starting at 28-bit LBA
 * `lba`, as one multi-sector PIO command rather than `count` single-sector
 * ones (SCRUM-190) -- the command issue, 400ns turnaround and command-block
 * register writes are paid once for the whole run instead of once per
 * sector. `buf` must point to (at least) count * 512 bytes. `count` must be
 * in [1, 255] (0 would select 256 sectors per the ATA spec encoding, which
 * nothing here needs); count == 0 is a no-op that returns ATA_OK without
 * touching hardware. Returns ATA_OK, or a negative ATA_* status. Same
 * precondition as the single-sector calls: ata_init() must have succeeded
 * first.
 */
int ata_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count);
int ata_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count);

/* Total user-addressable 28-bit LBA sector count, read out of the IDENTIFY
 * payload's words 60/61 (low word first) by a successful ata_init() --
 * SCRUM-202's `disk` shell command is the first caller. 0 if ata_init()
 * never succeeded (ATA_ENODEV/timeout) or has not been called yet. */
uint32_t ata_get_sector_count(void);

/* Sectors per DRQ data block the transfers above use: what ata_init()'s
 * SET MULTIPLE MODE was accepted with (READ/WRITE MULTIPLE), or 1 if the
 * drive has no multiple mode (plain READ/WRITE SECTORS). Exposed for the
 * test suite; nothing else needs to know. */
uint32_t ata_multiple_block(void);

/* Whether position `drive` (0..ATA_MAX_DRIVES-1) answered the last
 * ata_init() as an ATA disk, and its sector count (0 if not). */
int ata_drive_present(unsigned drive);
uint32_t ata_drive_sector_count(unsigned drive);

/* Register every present position with src/blkdev.h: the primary master in
 * the reserved system slot (block device 0), the others in position order
 * from slot 1 -- never in slot 0, even with no primary master. The block
 * ops are the only way anything above this driver reaches a drive other
 * than the primary master. Call once from kernel_main after ata_init().
 * Returns how many were registered. */
int ata_register_blkdevs(void);
