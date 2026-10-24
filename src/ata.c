#include "ata.h"
#include "blkdev.h"
#include "io.h"

#include <stddef.h>

/* Command-block register offsets from a bus's I/O base, and the two legacy
 * buses (SCRUM-102's own ticket note, 2026-09-19: QEMU `-drive if=ide` ->
 * port I/O, not SCSI). SCRUM-244 added the secondary bus and the slave
 * position on each, so a register is now `drive->io + ATA_REG_*` rather
 * than a fixed port. */
#define ATA_REG_DATA        0  /* 16-bit per the ATA spec; sector data is
                                 * moved 32 bits at a time -- see
                                 * ATA_DATA_DWORDS_PER_SECTOR */
#define ATA_REG_SECCOUNT    2
#define ATA_REG_LBA_LO      3
#define ATA_REG_LBA_MID     4
#define ATA_REG_LBA_HI      5
#define ATA_REG_DRIVE_HEAD  6
#define ATA_REG_STATUS      7  /* read */
#define ATA_REG_COMMAND     7  /* write */

#define ATA_PRIMARY_IO      0x1F0
#define ATA_PRIMARY_CTL     0x3F6  /* write: device control; read: alternate
                                     * status (same bits as STATUS, but
                                     * reading it never clears a pending IRQ) */
#define ATA_SECONDARY_IO    0x170
#define ATA_SECONDARY_CTL   0x376

/* Sector data moves through ATA_DATA as 32-bit `rep insl`/`rep outsl`
 * (SCRUM-190), half the port accesses of 16-bit `rep insw`. After READ
 * MULTIPLE, the WAD load's remaining time was QEMU dispatching each data
 * port access individually (perf: kvm_cpu_exec -> address_space_rw ->
 * ... -> ide_data_readw, ~14M of them), so halving the accesses roughly
 * halves the time.
 *
 * This is a QEMU assumption, not an ATA one. The data register and the
 * PATA cable are 16 bits wide; a 32-bit access works only because the host
 * controller splits it into two 16-bit cable transfers. PCI IDE
 * controllers such as the PIIX that QEMU emulates do; older ISA/VLB ones
 * did not, some chipsets needed enabling first, and nothing reliably
 * reports which (IDENTIFY word 48 described the drive, not the controller,
 * and is obsolete) -- which is why Linux leaves it off unless `hdparm -c`
 * asks. Where unsupported the result is garbage, not an error. Accepted
 * because this driver is QEMU-only in practice: real hardware has no PATA
 * and increasingly no IDE compatibility mode (SCRUM-242 is the real-
 * hardware disk path). IDENTIFY in ata_init() still uses 16-bit insw(),
 * so probing a drive assumes nothing beyond the spec. */
#define ATA_DATA_DWORDS_PER_SECTOR 128u

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

#define ATA_DRIVE_HEAD_LBA     0xE0  /* LBA mode, master drive select        */
#define ATA_DRIVE_HEAD_SLAVE   0x10  /* OR in to select the slave instead    */

/* What IDENTIFY DEVICE leaves in LBA_MID/LBA_HI when it is aborted by a
 * packet device (ATAPI) -- a CD-ROM. QEMU's `-cdrom` sits on the secondary
 * master, so the probe meets this on every boot and must not register it:
 * it is not a sector-addressed disk this driver can read. */
#define ATA_SIG_ATAPI_MID      0x14
#define ATA_SIG_ATAPI_HI       0xEB

/* IDENTIFY DEVICE word 47, bits 7:0: the most sectors the drive will move
 * per DRQ block under READ/WRITE MULTIPLE (0 = not supported). */
#define ATA_IDENTIFY_MULT_WORD 47

/* One IDE position (SCRUM-244). Indexed primary master, primary slave,
 * secondary master, secondary slave -- the order QEMU numbers `-drive
 * if=ide,index=N` in, and the order ata_register_blkdevs() registers them,
 * so the primary master is block device 0. */
typedef struct {
    uint16_t io;       /* command-block base                              */
    uint16_t ctl;      /* control-block port (device control / alt status) */
    uint8_t  select;   /* DRIVE_HEAD value: LBA mode + master/slave bit    */
    uint8_t  present;  /* answered IDENTIFY as an ATA disk                */
    /* Sectors per DRQ data block for every transfer below: the value
     * SET MULTIPLE MODE was accepted with, or 1 if the drive does not
     * support it (or refused it), in which case the plain READ/WRITE
     * SECTORS commands are used. Set by ata_init(); see there. */
    uint32_t block;
    /* Captured from IDENTIFY words 60/61 by ata_init() -- see
     * ata_get_sector_count()'s own comment in src/ata.h. Zero unless the
     * drive is present. */
    uint32_t sectors;
} ata_drive_t;

static ata_drive_t g_drives[ATA_MAX_DRIVES] = {
    { ATA_PRIMARY_IO,   ATA_PRIMARY_CTL,   ATA_DRIVE_HEAD_LBA,                        0, 1, 0 },
    { ATA_PRIMARY_IO,   ATA_PRIMARY_CTL,   ATA_DRIVE_HEAD_LBA | ATA_DRIVE_HEAD_SLAVE, 0, 1, 0 },
    { ATA_SECONDARY_IO, ATA_SECONDARY_CTL, ATA_DRIVE_HEAD_LBA,                        0, 1, 0 },
    { ATA_SECONDARY_IO, ATA_SECONDARY_CTL, ATA_DRIVE_HEAD_LBA | ATA_DRIVE_HEAD_SLAVE, 0, 1, 0 },
};

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
 * hardware. The same delay is what a drive-select needs before the newly
 * selected drive's status is valid (SCRUM-244), so ata_select() pays it too. */
static void ata_delay_400ns(const ata_drive_t *d) {
    (void)inb(d->ctl);
    (void)inb(d->ctl);
    (void)inb(d->ctl);
    (void)inb(d->ctl);
}

/* Poll until BSY clears. Returns ATA_OK, or ATA_ETIMEOUT if it never does. */
static int ata_wait_not_busy(const ata_drive_t *d) {
    for (int i = 0; i < ATA_POLL_ITERS; i++) {
        if ((inb(d->io + ATA_REG_STATUS) & ATA_STATUS_BSY) == 0) {
            return ATA_OK;
        }
    }
    return ATA_ETIMEOUT;
}

/* Poll until DRQ or ERR/DF is set (BSY already known clear). Returns ATA_OK
 * with DRQ set, ATA_EIO if the drive reported an error, or ATA_ETIMEOUT if
 * neither ever appears. */
static int ata_wait_drq(const ata_drive_t *d) {
    for (int i = 0; i < ATA_POLL_ITERS; i++) {
        uint8_t status = inb(d->io + ATA_REG_STATUS);
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
 * ata_select()/COMMAND for the *next* call, not just before
 * inspecting this command's result), then check ERR/DF. Returns ATA_OK,
 * ATA_ETIMEOUT if BSY never clears, or ATA_EIO if the drive reported an
 * error. */
static int ata_wait_ready(const ata_drive_t *d) {
    int rc = ata_wait_not_busy(d);
    if (rc != ATA_OK) {
        return rc;
    }
    uint8_t status = inb(d->io + ATA_REG_STATUS);
    if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
        return ATA_EIO;
    }
    return ATA_OK;
}

/* Program the drive/head + LBA + sector-count registers common to every
 * READ/WRITE SECTORS command, single- or multi-sector. Caller writes
 * COMMAND next. `count` is cast straight into the 8-bit SECCOUNT
 * register, so it must be in [1, 255] -- the only callers below cap well
 * under that (EXO_DISK_MAX_SECTORS is 128). Count 0 would mean "256
 * sectors" per the ATA spec, which nothing here needs or checks for.
 *
 * DRIVE_HEAD is written on every command, with this drive's master/slave
 * bit, because two drives share a bus's registers (SCRUM-244): the previous
 * command may have been for the other one. The 400ns delay after it lets
 * the newly selected drive drive the status lines before anyone polls. */
static void ata_select(const ata_drive_t *d, uint32_t lba, uint32_t count) {
    outb(d->io + ATA_REG_DRIVE_HEAD, d->select | ((lba >> 24) & 0x0F));
    ata_delay_400ns(d);
    outb(d->io + ATA_REG_SECCOUNT, (uint8_t)count);
    outb(d->io + ATA_REG_LBA_LO, (uint8_t)(lba & 0xFF));
    outb(d->io + ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(d->io + ATA_REG_LBA_HI, (uint8_t)((lba >> 16) & 0xFF));
}

/* IDENTIFY one position. Returns ATA_OK with d->present/block/sectors set,
 * ATA_ENODEV if nothing is there (or something is there that is not an ATA
 * disk -- an ATAPI CD-ROM), or ATA_ETIMEOUT/ATA_EIO on a drive that
 * responded but faulted. */
static int ata_probe(ata_drive_t *d) {
    d->present = 0;
    d->block = 1;
    d->sectors = 0;

    outb(d->io + ATA_REG_DRIVE_HEAD, d->select);
    ata_delay_400ns(d);

    /* 0x00: nothing wired at this position (QEMU reads an absent slave, and
     * an empty bus, as zero). 0xFF: a floating bus with no controller
     * pulling the lines -- real hardware's version of the same answer.
     * Either way, report rather than halt -- same convention
     * DG_Init/doom_wad.c use for a missing resource. */
    uint8_t status = inb(d->io + ATA_REG_STATUS);
    if (status == 0x00 || status == 0xFF) {
        return ATA_ENODEV;
    }

    outb(d->io + ATA_REG_SECCOUNT, 0);
    outb(d->io + ATA_REG_LBA_LO, 0);
    outb(d->io + ATA_REG_LBA_MID, 0);
    outb(d->io + ATA_REG_LBA_HI, 0);
    outb(d->io + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay_400ns(d);

    status = inb(d->io + ATA_REG_STATUS);
    if (status == 0) {
        return ATA_ENODEV;
    }

    int rc = ata_wait_not_busy(d);
    if (rc != ATA_OK) {
        return rc;
    }

    /* A packet device aborts IDENTIFY DEVICE and leaves its signature in
     * the LBA registers. Check before waiting for DRQ, which an aborted
     * command never raises: waiting would only find ERR, and treating the
     * CD-ROM as a faulted disk would make it look like a broken drive
     * rather than not-a-disk. */
    if (inb(d->io + ATA_REG_LBA_MID) == ATA_SIG_ATAPI_MID &&
        inb(d->io + ATA_REG_LBA_HI) == ATA_SIG_ATAPI_HI) {
        return ATA_ENODEV;
    }

    rc = ata_wait_drq(d);
    if (rc != ATA_OK) {
        return rc;
    }

    /* The 256-word IDENTIFY payload. Detection only needs the fact that it
     * arrived intact; the fields parsed are word 47 (below) and words 60/61
     * (the 28-bit-LBA total sector count, low word first -- the standard
     * ATA/ATAPI-7 IDENTIFY field), kept because SCRUM-202's `disk` shell
     * command is the first thing that reports capacity at all. */
    uint16_t id[256];
    insw(d->io + ATA_REG_DATA, id, 256);

    rc = ata_wait_ready(d);
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
     * (ERR -> ATA_EIO from ata_wait_ready()), keeps a block of 1, which
     * keeps the single-sector-per-DRQ commands -- slower, never wrong. */
    uint32_t mult = id[ATA_IDENTIFY_MULT_WORD] & 0xFF;
    if (mult > 1) {
        outb(d->io + ATA_REG_DRIVE_HEAD, d->select);
        outb(d->io + ATA_REG_SECCOUNT, (uint8_t)mult);
        outb(d->io + ATA_REG_COMMAND, ATA_CMD_SET_MULTIPLE);
        ata_delay_400ns(d);
        if (ata_wait_ready(d) == ATA_OK) {
            d->block = mult;
        }
    }
    d->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    d->present = 1;

    return ATA_OK;
}

/* Probes every position, not just the primary master (SCRUM-244). The
 * return value is still the primary master's own result, so every caller
 * that asked "is the system disk there?" before keeps asking it; whether
 * other drives answered is ata_drive_present(). */
int ata_init(void) {
    int rc0 = ATA_ENODEV;
    for (unsigned i = 0; i < ATA_MAX_DRIVES; i++) {
        int rc = ata_probe(&g_drives[i]);
        if (i == 0) {
            rc0 = rc;
        }
    }
    return rc0;
}

uint32_t ata_get_sector_count(void) {
    return g_drives[0].sectors;
}

uint32_t ata_multiple_block(void) {
    return g_drives[0].block;
}

int ata_drive_present(unsigned drive) {
    return drive < ATA_MAX_DRIVES && g_drives[drive].present;
}

uint32_t ata_drive_sector_count(unsigned drive) {
    return ata_drive_present(drive) ? g_drives[drive].sectors : 0;
}

/* Multi-sector PIO: the command, the LBA and the sector count are programmed
 * ONCE for the whole run (SCRUM-190) -- a fresh command would reset the
 * drive's internal auto-incrementing LBA, which is exactly what multi-sector
 * mode exists to avoid paying for per sector. The drive then signals
 * readiness (BSY clears, then DRQ sets) once per DRQ *block* of
 * d->block sectors -- READ/WRITE MULTIPLE when ata_probe() set multiple
 * mode, else READ/WRITE SECTORS with a block of one sector. The last block
 * of a run is short when `count` is not a multiple of the block size; the
 * drive sizes it the same way, so the loop just takes what is left.
 * `count` must be in [1, 255] (see ata_select()); callers below never
 * exceed EXO_DISK_MAX_SECTORS (128, src/syscall_disk.h). */
static int ata_read_on(const ata_drive_t *d, uint32_t lba, uint8_t *buf,
                       uint32_t count) {
    if (count == 0) {
        return ATA_OK;
    }
    if (!d->present) {
        return ATA_ENODEV;
    }

    uint32_t block = d->block;
    ata_select(d, lba, count);
    outb(d->io + ATA_REG_COMMAND, block > 1 ? ATA_CMD_READ_MULTIPLE
                                            : ATA_CMD_READ_SECTORS);
    ata_delay_400ns(d);

    for (uint32_t s = 0; s < count; s += block) {
        int rc = ata_wait_not_busy(d);
        if (rc != ATA_OK) {
            return rc;
        }

        rc = ata_wait_drq(d);
        if (rc != ATA_OK) {
            return rc;
        }

        uint32_t n = count - s < block ? count - s : block;

        /* One `rep insl` per DRQ block, not a loop of single INs: under
         * KVM each standalone IN is its own VM exit (insw() in io.h), and
         * 32-bit halves QEMU's per-access cost (ATA_DATA_DWORDS_PER_SECTOR). */
        insl(d->io + ATA_REG_DATA, buf + s * 512u, ATA_DATA_DWORDS_PER_SECTOR * n);
    }

    /* Let BSY settle before returning -- a drive may briefly reassert it
     * after the last data word, and the caller's next command-block write
     * (ata_select(), possibly on the very next call) must not race that. */
    return ata_wait_ready(d);
}

static int ata_write_on(const ata_drive_t *d, uint32_t lba,
                        const uint8_t *buf, uint32_t count) {
    if (count == 0) {
        return ATA_OK;
    }
    if (!d->present) {
        return ATA_ENODEV;
    }

    uint32_t block = d->block;
    ata_select(d, lba, count);
    outb(d->io + ATA_REG_COMMAND, block > 1 ? ATA_CMD_WRITE_MULTIPLE
                                            : ATA_CMD_WRITE_SECTORS);
    ata_delay_400ns(d);

    for (uint32_t s = 0; s < count; s += block) {
        int rc = ata_wait_not_busy(d);
        if (rc != ATA_OK) {
            return rc;
        }

        rc = ata_wait_drq(d);
        if (rc != ATA_OK) {
            return rc;
        }

        uint32_t n = count - s < block ? count - s : block;
        outsl(d->io + ATA_REG_DATA, buf + s * 512u, ATA_DATA_DWORDS_PER_SECTOR * n);
    }

    /* BSY must clear before the command-block registers (COMMAND
     * included) can be written again -- the drive may still be busy
     * committing the WRITE SECTORS data right after the last data word. */
    int rc = ata_wait_ready(d);
    if (rc != ATA_OK) {
        return rc;
    }

    /* CACHE FLUSH so a read of these same LBAs right after can see the
     * write without depending on QEMU's write-back timing (round-trip tests
     * do exactly this). ata_wait_ready() below both settles BSY and checks
     * ERR/DF, so a flush the drive actually rejected is reported as
     * ATA_EIO rather than silently coming back ATA_OK. The drive is still
     * selected from ata_select() above, so the flush goes to the same one. */
    outb(d->io + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_delay_400ns(d);
    return ata_wait_ready(d);
}

int ata_read_sectors(uint32_t lba, uint8_t *buf, uint32_t count) {
    return ata_read_on(&g_drives[0], lba, buf, count);
}

int ata_write_sectors(uint32_t lba, const uint8_t *buf, uint32_t count) {
    return ata_write_on(&g_drives[0], lba, buf, count);
}

int ata_read_sectors_on(unsigned drive, uint32_t lba, uint8_t *buf,
                        uint32_t count) {
    if (drive >= ATA_MAX_DRIVES) {
        return ATA_ENODEV;
    }
    return ata_read_on(&g_drives[drive], lba, buf, count);
}

int ata_write_sectors_on(unsigned drive, uint32_t lba, const uint8_t *buf,
                         uint32_t count) {
    if (drive >= ATA_MAX_DRIVES) {
        return ATA_ENODEV;
    }
    return ata_write_on(&g_drives[drive], lba, buf, count);
}

int ata_read_sector(uint32_t lba, uint8_t *buf) {
    return ata_read_sectors(lba, buf, 1);
}

int ata_write_sector(uint32_t lba, const uint8_t *buf) {
    return ata_write_sectors(lba, buf, 1);
}

/* ---- Block-device registration (SCRUM-244) ------------------------------ */

static int ata_status_to_blkdev(int rc) {
    switch (rc) {
    case ATA_OK:     return BLKDEV_OK;
    case ATA_ENODEV: return BLKDEV_ENODEV;
    default:         return BLKDEV_EIO;
    }
}

static int ata_blk_read(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count) {
    return ata_status_to_blkdev(ata_read_on((const ata_drive_t *)ctx, lba,
                                            buf, count));
}

static int ata_blk_write(void *ctx, uint32_t lba, const uint8_t *buf,
                         uint32_t count) {
    return ata_status_to_blkdev(ata_write_on((const ata_drive_t *)ctx, lba,
                                             buf, count));
}

int ata_register_blkdevs(void) {
    int n = 0;
    for (unsigned i = 0; i < ATA_MAX_DRIVES; i++) {
        if (!g_drives[i].present) {
            continue;
        }
        blkdev_t dev = {
            .sectors = g_drives[i].sectors,
            .read    = ata_blk_read,
            .write   = ata_blk_write,
            .ctx     = &g_drives[i],
        };
        if (blkdev_register(&dev) < 0) {
            break;
        }
        n++;
    }
    return n;
}
