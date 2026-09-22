#include "ata.h"
#include "io.h"

/* Primary ATA bus, port I/O command block + control block (SCRUM-102's own
 * ticket note, 2026-09-19: QEMU `-drive if=ide` -> port I/O, not SCSI). */
#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1  /* read */
#define ATA_FEATURES    0x1F1  /* write */
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LO      0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HI      0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7  /* read */
#define ATA_COMMAND     0x1F7  /* write */
#define ATA_CONTROL     0x3F6  /* write: device control; read: alternate
                                 * status (same bits as ATA_STATUS, but
                                 * reading it never clears a pending IRQ) */

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_BSY  0x80

#define ATA_CMD_READ_SECTORS   0x20
#define ATA_CMD_WRITE_SECTORS  0x30
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_IDENTIFY       0xEC

#define ATA_DRIVE_HEAD_LBA_MASTER 0xE0  /* LBA mode, master drive select   */

/* Bounded busy-wait, not a PIT-timed one: on real/QEMU hardware BSY/DRQ
 * resolve in microseconds, and tying this to kernel_get_ticks_ms() would
 * give ata.c a dependency on pit.c for no benefit -- see the ticket's plan
 * notes. Large enough that it never fires spuriously against QEMU, small
 * enough that a genuinely wedged drive doesn't hang boot forever. */
#define ATA_POLL_ITERS 100000

/* A drive does not assert BSY the instant it receives a new command -- the
 * well-known ~400ns turnaround (OSDev wiki, "ATA PIO Mode": "you need to
 * wait at least 400 nanoseconds ... before reading the Status register").
 * Polling ATA_STATUS immediately after outb(ATA_COMMAND, ...) can therefore
 * still observe the *previous* command's status (BSY already clear from
 * before this one was issued), which lets ata_wait_not_busy() return
 * ATA_OK before the new command has actually started -- intermittently, not
 * always, which is exactly the shape of the CI-only ata_write_sector()
 * failure this was added to fix (SCRUM-102: passed locally, failed under
 * CI's scheduling). Four throwaway reads of the alternate status register
 * (0x3F6, not 0x1F7 -- reading the primary status register can also clear a
 * pending IRQ, which this must not do) is the standard way to force that
 * delay: each I/O read takes on the order of 100ns on real hardware. */
static void ata_delay_400ns(void) {
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
    (void)inb(ATA_CONTROL);
}

/* Poll until BSY clears. Returns ATA_OK, or ATA_ETIMEOUT if it never does. */
static int ata_wait_not_busy(void) {
    for (int i = 0; i < ATA_POLL_ITERS; i++) {
        if ((inb(ATA_STATUS) & ATA_STATUS_BSY) == 0) {
            return ATA_OK;
        }
    }
    return ATA_ETIMEOUT;
}

/* Poll until DRQ or ERR/DF is set (BSY already known clear). Returns ATA_OK
 * with DRQ set, ATA_EIO if the drive reported an error, or ATA_ETIMEOUT if
 * neither ever appears. */
static int ata_wait_drq(void) {
    for (int i = 0; i < ATA_POLL_ITERS; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            return ATA_EIO;
        }
        if (status & ATA_STATUS_DRQ) {
            return ATA_OK;
        }
    }
    return ATA_ETIMEOUT;
}

/* Wait for the drive to settle after a command: poll BSY clear (the ATA
 * protocol forbids writing the command-block registers -- drive/head,
 * sector count, LBA, command -- while BSY is set, so this must run before
 * ata_select_sector()/ATA_COMMAND for the *next* call, not just before
 * inspecting this command's result), then check ERR/DF. Returns ATA_OK,
 * ATA_ETIMEOUT if BSY never clears, or ATA_EIO if the drive reported an
 * error. */
static int ata_wait_ready(void) {
    int rc = ata_wait_not_busy();
    if (rc != ATA_OK) {
        return rc;
    }
    uint8_t status = inb(ATA_STATUS);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return ATA_EIO;
    }
    return ATA_OK;
}

/* Program the drive/head + LBA registers and sector count = 1 common to
 * every single-sector command. Caller writes ATA_COMMAND next. */
static void ata_select_sector(uint32_t lba) {
    outb(ATA_DRIVE_HEAD, ATA_DRIVE_HEAD_LBA_MASTER | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    outb(ATA_DRIVE_HEAD, ATA_DRIVE_HEAD_LBA_MASTER);
    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns();

    uint8_t status = inb(ATA_STATUS);
    if (status == 0) {
        /* Floating bus: no drive wired at all. Report, don't halt -- same
         * convention DG_Init/doom_wad.c use for a missing resource. */
        return ATA_ENODEV;
    }

    int rc = ata_wait_not_busy();
    if (rc != ATA_OK) {
        return rc;
    }

    rc = ata_wait_drq();
    if (rc != ATA_OK) {
        return rc;
    }

    /* Drain the 256-word IDENTIFY payload. Detection only needs the fact
     * that it arrived intact; nothing here parses model/serial/capacity. */
    for (int i = 0; i < 256; i++) {
        (void)inw(ATA_DATA);
    }

    return ATA_OK;
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    ata_select_sector(lba);
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);
    ata_delay_400ns();

    int rc = ata_wait_not_busy();
    if (rc != ATA_OK) {
        return rc;
    }

    rc = ata_wait_drq();
    if (rc != ATA_OK) {
        return rc;
    }

    for (int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_DATA);
        buf[2 * i] = (uint8_t)(word & 0xFF);
        buf[2 * i + 1] = (uint8_t)(word >> 8);
    }

    /* Let BSY settle before returning -- a drive may briefly reassert it
     * after the last data word, and the caller's next command-block write
     * (ata_select_sector(), possibly on the very next call) must not race
     * that. */
    return ata_wait_ready();
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    ata_select_sector(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
    ata_delay_400ns();

    int rc = ata_wait_not_busy();
    if (rc != ATA_OK) {
        return rc;
    }

    rc = ata_wait_drq();
    if (rc != ATA_OK) {
        return rc;
    }

    for (int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buf[2 * i] | ((uint16_t)buf[2 * i + 1] << 8);
        outw(ATA_DATA, word);
    }

    /* BSY must clear before the command-block registers (ATA_COMMAND
     * included) can be written again -- the drive may still be busy
     * committing the WRITE SECTORS data right after the last data word. */
    rc = ata_wait_ready();
    if (rc != ATA_OK) {
        return rc;
    }

    /* CACHE FLUSH so a read of this same LBA right after can see the write
     * without depending on QEMU's write-back timing (round-trip tests do
     * exactly this). ata_wait_ready() below both settles BSY and checks
     * ERR/DF, so a flush the drive actually rejected is reported as
     * ATA_EIO rather than silently coming back ATA_OK. */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay_400ns();
    return ata_wait_ready();
}
