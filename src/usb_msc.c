/*
 * usb_msc.c — USB mass storage, Bulk-Only Transport + SCSI (SCRUM-247).
 *
 * See usb_msc.h and docs/drivers/usb.md. A command is three bulk phases:
 *
 *   CBW (31 bytes, OUT)  ->  data (IN or OUT, optional)  ->  CSW (13 bytes, IN)
 *
 * and any phase that goes wrong ends in reset recovery: the class Mass
 * Storage Reset request, then CLEAR_FEATURE(ENDPOINT_HALT) on both bulk
 * endpoints, with both data toggles back at DATA0. The one exception is a
 * STALL on the data or status phase, which the spec lets us clear on that
 * endpoint and carry on to the CSW.
 */

#include "usb_msc.h"
#include "blkdev.h"
#include "serial.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

#define CBW_SIGNATURE 0x43425355u    /* "USBC", little endian */
#define CSW_SIGNATURE 0x53425355u    /* "USBS" */
#define CBW_SIZE      31
#define CSW_SIZE      13
#define CBW_DIR_IN    0x80

#define CSW_STATUS_PASSED 0
#define CSW_STATUS_FAILED 1

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
} cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;
} csw_t;

_Static_assert(sizeof(cbw_t) == CBW_SIZE, "CBW is 31 bytes");
_Static_assert(sizeof(csw_t) == CSW_SIZE, "CSW is 13 bytes");

/* SCSI operation codes (SPC-4 / SBC-3). */
#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_REQUEST_SENSE   0x03
#define SCSI_INQUIRY         0x12
#define SCSI_READ_CAPACITY10 0x25
#define SCSI_READ10          0x28
#define SCSI_WRITE10         0x2A

#define SENSE_NOT_READY      0x02
#define SENSE_UNIT_ATTENTION 0x06

/* INQUIRY peripheral device types we accept: direct-access block device,
 * and simplified direct-access (RBC), which flash drives often report. */
#define SCSI_TYPE_DIRECT     0x00
#define SCSI_TYPE_RBC        0x0E

/* TEST UNIT READY: a stick can take a moment to spin up, and answers
 * NOT READY or UNIT ATTENTION until it has. */
#define READY_TRIES      50
#define READY_DELAY_MS   100

#define MAX_SECTORS_PER_CMD 128    /* EXO_DISK_MAX_SECTORS */
#define IO_ATTEMPTS         2

static usb_msc_t g_msc[USB_MSC_MAX];
static int       g_nmsc;

/* The wrappers live in static storage rather than on the stack: it is
 * kernel .bss, below 4 GB on the identity map, which is what a 32-bit
 * controller needs of every buffer it reads or writes. One command runs at
 * a time (everything here is busy-waited from a single context), so one
 * pair is enough. */
static cbw_t g_cbw;
static csw_t g_csw;

/* Sector data goes through this bounce buffer, never straight to or from the
 * caller's. A block-device op is handed whatever pointer the syscall layer
 * had, and for a ring-3 context that is a user virtual address (its heap sits
 * above the identity map), which means nothing to a DMA engine: a TD needs a
 * physical address below 4 GB. Kernel .bss is both. ATA PIO never cared,
 * because the CPU does the copy; this does it once, which at ~1 MB/s is
 * noise. */
static uint8_t g_bounce[MAX_SECTORS_PER_CMD * USB_MSC_SECTOR];

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}

/* ── Bulk-Only Transport ──────────────────────────────────────────────── */

void usb_msc_reset_recovery(usb_msc_t *m)
{
    usb_control(m->dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE,
                USB_MSC_REQ_RESET, 0, m->iface, NULL, 0, NULL);
    usb_clear_halt(m->dev, m->in_ep);
    usb_clear_halt(m->dev, m->out_ep);
    m->in_toggle = 0;
    m->out_toggle = 0;
}

/* One bulk phase with a single STALL recovery: clear the halt, reset that
 * endpoint's toggle, and report the stall to the caller. */
static int phase(usb_msc_t *m, uint8_t ep, void *buf, uint32_t len,
                 uint32_t *actual)
{
    int in = (ep & USB_EP_DIR_IN) != 0;
    uint8_t *toggle = in ? &m->in_toggle : &m->out_toggle;
    uint16_t mps = in ? m->in_mps : m->out_mps;
    int rc = usb_bulk(m->dev, ep, mps, toggle, buf, len, actual);

    if (rc == USB_ESTALL) {
        usb_clear_halt(m->dev, ep);
        *toggle = 0;
    }
    return rc;
}

int usb_msc_command(usb_msc_t *m, const uint8_t *cdb, uint8_t cdb_len,
                    int dir_in, void *data, uint32_t len)
{
    uint32_t moved = 0;
    int rc;

    if (cdb_len == 0 || cdb_len > sizeof(g_cbw.cb) || (len && data == NULL)) {
        return USB_MSC_EIO;
    }

    memset(&g_cbw, 0, sizeof(g_cbw));
    g_cbw.signature = CBW_SIGNATURE;
    g_cbw.tag = ++m->tag;
    g_cbw.data_len = len;
    g_cbw.flags = (len && dir_in) ? CBW_DIR_IN : 0;
    g_cbw.cb_len = cdb_len;
    memcpy(g_cbw.cb, cdb, cdb_len);

    rc = phase(m, m->out_ep, &g_cbw, CBW_SIZE, &moved);
    if (rc != USB_OK || moved != CBW_SIZE) {
        usb_msc_reset_recovery(m);
        return USB_MSC_EIO;
    }

    if (len) {
        rc = phase(m, dir_in ? m->in_ep : m->out_ep, data, len, &moved);
        /* A stall is the device ending the data phase early; the CSW still
         * follows. Any other failure leaves the transport in an unknown
         * state. */
        if (rc != USB_OK && rc != USB_ESTALL) {
            usb_msc_reset_recovery(m);
            return USB_MSC_EIO;
        }
    }

    /* The CSW; one retry after a STALL on the status phase (BOT 6.7.2). */
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(&g_csw, 0, sizeof(g_csw));
        rc = phase(m, m->in_ep, &g_csw, CSW_SIZE, &moved);
        if (rc != USB_ESTALL) {
            break;
        }
    }
    if (rc != USB_OK || moved != CSW_SIZE
        || g_csw.signature != CSW_SIGNATURE || g_csw.tag != g_cbw.tag) {
        usb_msc_reset_recovery(m);
        return USB_MSC_EIO;
    }

    switch (g_csw.status) {
    case CSW_STATUS_PASSED:
        return USB_MSC_OK;
    case CSW_STATUS_FAILED:
        return USB_MSC_CHECK;
    default:                          /* phase error, or reserved */
        usb_msc_reset_recovery(m);
        return USB_MSC_EIO;
    }
}

/* ── SCSI ─────────────────────────────────────────────────────────────── */

/* After CHECK CONDITION: the sense key, or -1 if REQUEST SENSE itself
 * failed. Also what clears a UNIT ATTENTION. */
static int request_sense(usb_msc_t *m)
{
    uint8_t cdb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    static uint8_t sense[18];

    memset(sense, 0, sizeof(sense));
    if (usb_msc_command(m, cdb, sizeof(cdb), 1, sense, sizeof(sense))
        != USB_MSC_OK) {
        return -1;
    }
    return sense[2] & 0x0F;
}

static int wait_ready(usb_msc_t *m)
{
    uint8_t cdb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };

    for (int i = 0; i < READY_TRIES; i++) {
        int rc = usb_msc_command(m, cdb, sizeof(cdb), 0, NULL, 0);
        if (rc == USB_MSC_OK) {
            return USB_OK;
        }
        if (rc < 0) {
            return USB_EIO;
        }
        int key = request_sense(m);
        if (key != SENSE_NOT_READY && key != SENSE_UNIT_ATTENTION) {
            return USB_EIO;
        }
        m->dev->hcd->ops->delay_ms(m->dev->hcd, READY_DELAY_MS);
    }
    return USB_ETIMEOUT;
}

static int rw10(usb_msc_t *m, int write, uint32_t lba, void *buf,
                uint32_t count)
{
    uint8_t cdb[10] = { write ? SCSI_WRITE10 : SCSI_READ10, 0 };

    put_be32(&cdb[2], lba);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    return usb_msc_command(m, cdb, sizeof(cdb), !write, buf,
                           count * USB_MSC_SECTOR);
}

static int msc_transfer(void *ctx, int write, uint32_t lba, void *buf,
                        uint32_t count)
{
    usb_msc_t *m = ctx;

    if (count == 0 || count > MAX_SECTORS_PER_CMD) {
        return BLKDEV_EIO;
    }
    uint32_t bytes = count * USB_MSC_SECTOR;

    if (write) {
        memcpy(g_bounce, buf, bytes);
    }
    for (int attempt = 0; attempt < IO_ATTEMPTS; attempt++) {
        int rc = rw10(m, write, lba, g_bounce, count);

        if (rc == USB_MSC_OK) {
            if (!write) {
                memcpy(buf, g_bounce, bytes);
            }
            return BLKDEV_OK;
        }
        if (rc == USB_MSC_CHECK) {
            request_sense(m);         /* clears the condition before a retry */
        }
    }
    return BLKDEV_EIO;
}

int usb_msc_read(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count)
{
    return msc_transfer(ctx, 0, lba, buf, count);
}

int usb_msc_write(void *ctx, uint32_t lba, const uint8_t *buf, uint32_t count)
{
    return msc_transfer(ctx, 1, lba, (void *)buf, count);
}

/* ── Probe ────────────────────────────────────────────────────────────── */

static void log_msc(const char *msg)
{
    serial_print("usb_msc: ");
    serial_print(msg);
}

static int read_capacity(usb_msc_t *m, uint32_t *sectors)
{
    uint8_t cdb[10] = { SCSI_READ_CAPACITY10 };
    static uint8_t cap[8];

    memset(cap, 0, sizeof(cap));
    if (usb_msc_command(m, cdb, sizeof(cdb), 1, cap, sizeof(cap))
        != USB_MSC_OK) {
        return USB_EIO;
    }
    uint32_t last = get_be32(cap);
    uint32_t block = get_be32(cap + 4);

    if (block != USB_MSC_SECTOR) {
        log_msc("block size is not 512 -- ");
        return USB_ENOTSUP;
    }
    if (last == 0xFFFFFFFFu) {         /* needs READ CAPACITY(16): > 2 TiB */
        log_msc("capacity beyond 32-bit LBA -- ");
        return USB_ENOTSUP;
    }
    *sectors = last + 1;
    return USB_OK;
}

int usb_msc_probe(usb_device_t *dev, const uint8_t *config, uint16_t len)
{
    const usb_interface_desc_t *iface;
    const usb_endpoint_desc_t *bin;
    const usb_endpoint_desc_t *bout;

    if (usb_config_find_interface(config, len, USB_CLASS_MASS_STORAGE,
                                  USB_MSC_SUBCLASS_SCSI, USB_MSC_PROTO_BOT,
                                  &iface, NULL) != USB_OK) {
        return 0;
    }
    if (usb_config_find_bulk(config, len, iface, &bin, &bout) != USB_OK) {
        log_msc("no bulk endpoint pair -- ");
        return 0;
    }
    if (g_nmsc >= USB_MSC_MAX) {
        log_msc("table full -- ");
        return 0;
    }

    uint16_t in_mps = bin->wMaxPacketSize & 0x7FF;
    uint16_t out_mps = bout->wMaxPacketSize & 0x7FF;
    if (dev->speed == USB_SPEED_LOW || in_mps < 8 || in_mps > 64
        || out_mps < 8 || out_mps > 64) {
        log_msc("unsupported endpoint size -- ");
        return 0;
    }

    int rc = usb_set_configuration(dev, dev->config_value);
    if (rc != USB_OK) {
        usb_log_rc("usb_msc: SET_CONFIGURATION", rc);
        return 0;
    }

    usb_msc_t *m = &g_msc[g_nmsc];
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->iface = iface->bInterfaceNumber;
    m->in_ep = bin->bEndpointAddress;
    m->out_ep = bout->bEndpointAddress;
    m->in_mps = in_mps;
    m->out_mps = out_mps;

    uint8_t inq_cdb[6] = { SCSI_INQUIRY, 0, 0, 0, 36, 0 };
    static uint8_t inq[36];
    memset(inq, 0, sizeof(inq));
    if (usb_msc_command(m, inq_cdb, sizeof(inq_cdb), 1, inq, sizeof(inq))
        != USB_MSC_OK) {
        log_msc("INQUIRY failed -- ");
        return 0;
    }
    uint8_t type = inq[0] & 0x1F;
    if (type != SCSI_TYPE_DIRECT && type != SCSI_TYPE_RBC) {
        log_msc("not a block device -- ");
        return 0;
    }

    if ((rc = wait_ready(m)) != USB_OK) {
        usb_log_rc("usb_msc: unit ready", rc);
        return 0;
    }
    if (read_capacity(m, &m->sectors) != USB_OK) {
        log_msc("READ CAPACITY failed -- ");
        return 0;
    }

    blkdev_t bd = {
        .sectors = m->sectors,
        .read    = usb_msc_read,
        .write   = usb_msc_write,
        .ctx     = m,
    };
    int idx = blkdev_register(&bd);
    if (idx < 0) {
        log_msc("no block-device slot -- ");
        return 0;
    }
    g_nmsc++;

    log_msc("block device ");
    serial_print_dec((uint32_t)idx);
    serial_print(", ");
    serial_print_dec(m->sectors);
    serial_print(" sectors\n");
    return 1;
}

int usb_msc_count(void)
{
    return g_nmsc;
}

const usb_msc_t *usb_msc_at(int idx)
{
    return (idx >= 0 && idx < g_nmsc) ? &g_msc[idx] : NULL;
}
