#pragma once
#include <stdint.h>

/*
 * ata — polled PIO driver for the primary ATA bus, master drive (SCRUM-102).
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
 * as unnecessary complexity for a QEMU-only demo. This driver only ever
 * speaks to that one drive: no slave, no secondary bus, no drive-select
 * parameter on the public API.
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

/* Probe the primary master with IDENTIFY. Returns ATA_OK if a drive answered,
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
 * successful ata_init().
 */
int ata_read_sector(uint32_t lba, uint8_t *buf);
int ata_write_sector(uint32_t lba, const uint8_t *buf);
