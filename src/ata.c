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
#define ATA_CMD_READ_MULTIPLE  0xC4
#define ATA_CMD_WRITE_MULTIPLE 0xC5
#define ATA_CMD_SET_MULTIPLE   0xC6
#define ATA_CMD_CACHE_FLUSH    0xE7
#define ATA_CMD_IDENTIFY       0xEC

#define ATA_DRIVE_HEAD_LBA_MASTER 0xE0  /* LBA mode, master drive select   */

/* IDENTIFY DEVICE word 47, bits 7:0: the most sectors the drive will move
 * per DRQ block under READ/WRITE MULTIPLE (0 = not supported). */
#define ATA_IDENTIFY_MULT_WORD 47

/* Sectors per DRQ data block for every transfer below: the value
 * SET MULTIPLE MODE was accepted with, or 1 if the drive does not support
 * it (or refused it), in which case the plain READ/WRITE SECTORS commands
 * are used. Set by ata_init(); see there. */
static uint32_t g_ata_block = 1;

/* Bounded busy-wait, not a PIT-timed one: on real/QEMU hardware BSY/DRQ
 * resolve in microseconds, and tying this to kernel_get_ticks_ms() would
 * give ata.c a dependency on pit.c for no benefit -- see the ticket's plan
 * notes. 10,000,000, not 100,000 (SCRUM-102, PR #122): CI's shared runners
 * reproducibly hit ATA_ETIMEOUT at the smaller bound on a genuinely working
 * drive -- confirmed by printing the real rc, not guessed -- while three
 * local docker-test runs never came close to either bound. Both local and
 * CI QEMU run under the same software emulation (docker/scripts never pass
 * /dev/kvm through), so this is CI's shared-runner contention costing real
 * wall-clock time per busy-spin iteration, not a difference in what's being
 * emulated. Still bounded, not unbounded: a genuinely wedged drive reports
 * ATA_ETIMEOUT rather than hanging boot forever, just after a larger budget
 * than the smaller constant gave local runs credit for. */
#define ATA_POLL_ITERS 10000000

/* A drive does not assert BSY the instant it receives a new command -- the
 * well-known ~400ns turnaround (OSDev wiki, "ATA PIO Mode": "you need to
 * wait at least 400 nanoseconds ... before reading the Status register").
 * Polling ATA_STATUS immediately after outb(ATA_COMMAND, ...) can therefore
 * still observe the *previous* command's status (BSY already clear from
 * before this one was issued), which would let ata_wait_not_busy() return
 * ATA_OK before the new command has actually started. Correct per spec
 * regardless, but note this alone did *not* explain PR #122's CI failure --
 * that turned out to be ATA_ETIMEOUT from ATA_POLL_ITERS being too small
 * for CI's contention, fixed above. Four throwaway reads of the alternate
 * status register (0x3F6, not 0x1F7 -- reading the primary status register
 * can also clear a pending IRQ, which this must not do) is the standard way
 * to force the delay: each I/O read takes on the order of 100ns on real
 * hardware. */
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
 * ata_select()/ATA_COMMAND for the *next* call, not just before
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

/* Program the drive/head + LBA + sector-count registers common to every
 * READ/WRITE SECTORS command, single- or multi-sector. Caller writes
 * ATA_COMMAND next. `count` is cast straight into the 8-bit SECCOUNT
 * register, so it must be in [1, 255] -- the only callers below cap well
 * under that (EXO_DISK_MAX_SECTORS is 128). Count 0 would mean "256
 * sectors" per the ATA spec, which nothing here needs or checks for. */
static void ata_select(uint32_t lba, uint32_t count) {
    outb(ATA_DRIVE_HEAD, ATA_DRIVE_HEAD_LBA_MASTER | ((lba >> 24) & 0x0F));
    outb(ATA_SECCOUNT, (uint8_t)count);
    outb(ATA_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    g_ata_block = 1;

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

    /* The 256-word IDENTIFY payload. Detection only needs the fact that it
     * arrived intact; the one field parsed is word 47, below. */
    uint16_t id[256];
    insw(ATA_DATA, id, 256);

    rc = ata_wait_ready();
    if (rc != ATA_OK) {
        return rc;
    }

    /* SET MULTIPLE MODE to the drive's own maximum (SCRUM-190). With plain
     * READ/WRITE SECTORS the drive raises DRQ once per 512-byte sector, and
     * every one of those handshakes -- a BSY poll, a DRQ poll and a
     * `rep insw` -- is a VM exit to QEMU's device model under KVM, ~5 per
     * sector measured over the 28 MB WAD load; QEMU also issues one block
     * request per DRQ block. A multiple-mode block of N sectors pays that
     * once per N. A drive that reports no support, or rejects the command
     * (ERR -> ATA_EIO from ata_wait_ready()), leaves g_ata_block at 1, which
     * keeps the single-sector-per-DRQ commands -- slower, never wrong. */
    uint32_t mult = id[ATA_IDENTIFY_MULT_WORD] & 0xFF;
    if (mult > 1) {
        outb(ATA_DRIVE_HEAD, ATA_DRIVE_HEAD_LBA_MASTER);
        outb(ATA_SECCOUNT, (uint8_t)mult);
        outb(ATA_COMMAND, ATA_CMD_SET_MULTIPLE);
        ata_delay_400ns();
        if (ata_wait_ready() == ATA_OK) {
            g_ata_block = mult;
        }
    }

    return ATA_OK;
}

uint32_t ata_multiple_block(void) {
    return g_ata_block;
}

/* Multi-sector PIO: the command, the LBA and the sector count are programmed
 * ONCE for the whole run (SCRUM-190) -- a fresh command would reset the
 * drive's internal auto-incrementing LBA, which is exactly what multi-sector
 * mode exists to avoid paying for per sector. The drive then signals
 * readiness (BSY clears, then DRQ sets) once per DRQ *block* of
 * g_ata_block sectors -- READ/WRITE MULTIPLE when ata_init() set multiple
 * mode, else READ/WRITE SECTORS with a block of one sector. The last block
 * of a run is short when `count` is not a multiple of the block size; the
 * drive sizes it the same way, so the loop just takes what is left.
 * `count` must be in [1, 255] (see ata_select()); callers below never
 * exceed EXO_DISK_MAX_SECTORS (128, src/syscall_disk.h). */
int ata_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count) {
    if (count == 0) {
        return ATA_OK;
    }

    uint32_t block = g_ata_block;
    ata_select(lba, count);
    outb(ATA_COMMAND, block > 1 ? ATA_CMD_READ_MULTIPLE
                                : ATA_CMD_READ_SECTORS);
    ata_delay_400ns();

    for (uint32_t s = 0; s < count; s += block) {
        int rc = ata_wait_not_busy();
        if (rc != ATA_OK) {
            return rc;
        }

        rc = ata_wait_drq();
        if (rc != ATA_OK) {
            return rc;
        }

        uint32_t n = count - s < block ? count - s : block;

        /* One `rep insw` per DRQ block, not 256 inw()s per sector: under
         * KVM each standalone IN is its own VM exit, see insw() in io.h. */
        insw(ATA_DATA, buf + s * 512u, 256u * n);
    }

    /* Let BSY settle before returning -- a drive may briefly reassert it
     * after the last data word, and the caller's next command-block write
     * (ata_select(), possibly on the very next call) must not race that. */
    return ata_wait_ready();
}

int ata_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count) {
    if (count == 0) {
        return ATA_OK;
    }

    uint32_t block = g_ata_block;
    ata_select(lba, count);
    outb(ATA_COMMAND, block > 1 ? ATA_CMD_WRITE_MULTIPLE
                                : ATA_CMD_WRITE_SECTORS);
    ata_delay_400ns();

    for (uint32_t s = 0; s < count; s += block) {
        int rc = ata_wait_not_busy();
        if (rc != ATA_OK) {
            return rc;
        }

        rc = ata_wait_drq();
        if (rc != ATA_OK) {
            return rc;
        }

        uint32_t n = count - s < block ? count - s : block;
        outsw(ATA_DATA, buf + s * 512u, 256u * n);
    }

    /* BSY must clear before the command-block registers (ATA_COMMAND
     * included) can be written again -- the drive may still be busy
     * committing the WRITE SECTORS data right after the last data word. */
    int rc = ata_wait_ready();
    if (rc != ATA_OK) {
        return rc;
    }

    /* CACHE FLUSH so a read of these same LBAs right after can see the
     * write without depending on QEMU's write-back timing (round-trip tests
     * do exactly this). ata_wait_ready() below both settles BSY and checks
     * ERR/DF, so a flush the drive actually rejected is reported as
     * ATA_EIO rather than silently coming back ATA_OK. */
    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay_400ns();
    return ata_wait_ready();
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    return ata_read_sectors(lba, buf, 1);
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    return ata_write_sectors(lba, buf, 1);
}
