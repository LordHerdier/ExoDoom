/*
 * test_usb_msc_k.c — USB mass storage: Bulk-Only Transport + SCSI
 * (SCRUM-247 acceptance).
 *
 * Two halves. The first runs against the `usb-storage` stick every QEMU line
 * attaches (QEMU_USB_ARGS in the Makefile: uhci3 port 1, backed by
 * build/usb.img, an ExoFS volume of the .exo packages from LBA 0): the unit is
 * claimed and registered as a block device, a known sector reads back, a
 * sector written reads back, and a 64 KiB read -- many bulk chunks -- matches
 * the same sectors read one at a time.
 *
 * The second half drives the transport over a fake controller, because a bad
 * CSW or a STALL cannot be provoked on QEMU's well-behaved device. The fake
 * answers the three bulk phases from a script and counts what recovery
 * sends.
 *
 * ORDER. Registered right after "blkdev", ahead of the suites that format the
 * whole stick (multidisk, shell_install): the superblock check needs the
 * packer's volume still at LBA 0. Everything this suite writes it puts back.
 */

#include "kunit.h"
#include "usb.h"
#include "usb_msc.h"
#include "blkdev.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

#define EXOFS_MAGIC_WORD 0x53464F58u    /* src/libos_fs/exofs_layout.h */

#define CHUNKED_SECTORS 128u            /* one blkdev op: EXO_DISK_MAX_SECTORS */

static uint8_t g_buf_a[CHUNKED_SECTORS * 512u];
static uint8_t g_buf_b[CHUNKED_SECTORS * 512u];

/* The registry slot holding the first claimed unit, or -1. */
static int stick_slot(void)
{
    const usb_msc_t *m = usb_msc_at(0);

    for (uint32_t i = 0; m && i < BLKDEV_MAX; i++) {
        const blkdev_t *bd = blkdev_get(i);
        if (bd && bd->ctx == m) {
            return (int)i;
        }
    }
    return -1;
}

/* ---- The real stick ------------------------------------------------------ */

static void test_stick_claimed_and_registered(void)
{
    int claimed = 0;

    for (int i = 0; i < usb_device_count(); i++) {
        const usb_device_t *d = usb_device_at(i);
        if (d->driver && strcmp(d->driver, "usb_msc") == 0) {
            claimed++;
            CU_ASSERT_EQUAL(d->iface_class, USB_CLASS_MASS_STORAGE);
            CU_ASSERT_EQUAL(d->iface_protocol, USB_MSC_PROTO_BOT);
        }
    }
    CU_ASSERT_EQUAL(claimed, 1);
    CU_ASSERT_EQUAL(usb_msc_count(), 1);

    int slot = stick_slot();
    CU_ASSERT_TRUE(slot >= 1);              /* never the system slot */
    const blkdev_t *bd = blkdev_get((uint32_t)slot);
    CU_ASSERT_PTR_NOT_NULL(bd);
    if (bd) {
        CU_ASSERT_TRUE(bd->sectors >= 8192u);
        CU_ASSERT_EQUAL(bd->sectors, usb_msc_at(0)->sectors);
    }
}

static void test_known_sector_reads(void)
{
    int slot = stick_slot();
    const blkdev_t *bd = blkdev_get((uint32_t)slot);

    CU_ASSERT_PTR_NOT_NULL(bd);
    if (!bd) {
        return;
    }
    memset(g_buf_a, 0, 512);
    CU_ASSERT_EQUAL(bd->read(bd->ctx, 0, g_buf_a, 1), BLKDEV_OK);

    uint32_t magic;
    memcpy(&magic, g_buf_a, sizeof(magic));
    CU_ASSERT_EQUAL(magic, EXOFS_MAGIC_WORD);
}

static void test_write_then_read_back(void)
{
    int slot = stick_slot();
    const blkdev_t *bd = blkdev_get((uint32_t)slot);

    CU_ASSERT_PTR_NOT_NULL(bd);
    if (!bd) {
        return;
    }
    uint32_t lba = bd->sectors - 2;         /* two sectors, near the end */

    CU_ASSERT_EQUAL(bd->read(bd->ctx, lba, g_buf_a, 2), BLKDEV_OK);

    for (uint32_t i = 0; i < 1024; i++) {
        g_buf_b[i] = (uint8_t)(i * 7 + 3);
    }
    CU_ASSERT_EQUAL(bd->write(bd->ctx, lba, g_buf_b, 2), BLKDEV_OK);

    static uint8_t back[1024];
    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(bd->read(bd->ctx, lba, back, 2), BLKDEV_OK);
    CU_ASSERT_EQUAL(memcmp(back, g_buf_b, 1024), 0);

    /* Put the original back. */
    CU_ASSERT_EQUAL(bd->write(bd->ctx, lba, g_buf_a, 2), BLKDEV_OK);
    memset(back, 0, sizeof(back));
    CU_ASSERT_EQUAL(bd->read(bd->ctx, lba, back, 2), BLKDEV_OK);
    CU_ASSERT_EQUAL(memcmp(back, g_buf_a, 1024), 0);
}

/* 128 sectors is 1024 full-speed packets: several bulk chunks of one
 * command. It must equal the same range fetched a sector at a time. */
static void test_large_read_matches_single_sector_reads(void)
{
    int slot = stick_slot();
    const blkdev_t *bd = blkdev_get((uint32_t)slot);

    CU_ASSERT_PTR_NOT_NULL(bd);
    if (!bd) {
        return;
    }
    memset(g_buf_a, 0xAA, sizeof(g_buf_a));
    CU_ASSERT_EQUAL(bd->read(bd->ctx, 0, g_buf_a, CHUNKED_SECTORS), BLKDEV_OK);

    int same = 1;
    for (uint32_t s = 0; s < CHUNKED_SECTORS; s++) {
        static uint8_t one[512];
        if (bd->read(bd->ctx, s, one, 1) != BLKDEV_OK
            || memcmp(one, g_buf_a + s * 512u, 512) != 0) {
            same = 0;
            break;
        }
    }
    CU_ASSERT_TRUE(same);
}

/* A read past the end fails with CHECK CONDITION; the unit must still
 * answer afterwards. */
static void test_read_past_end_fails_then_recovers(void)
{
    int slot = stick_slot();
    const blkdev_t *bd = blkdev_get((uint32_t)slot);

    CU_ASSERT_PTR_NOT_NULL(bd);
    if (!bd) {
        return;
    }
    CU_ASSERT_EQUAL(bd->read(bd->ctx, bd->sectors, g_buf_a, 1), BLKDEV_EIO);
    memset(g_buf_a, 0, 512);
    CU_ASSERT_EQUAL(bd->read(bd->ctx, 0, g_buf_a, 1), BLKDEV_OK);

    uint32_t magic;
    memcpy(&magic, g_buf_a, sizeof(magic));
    CU_ASSERT_EQUAL(magic, EXOFS_MAGIC_WORD);
}

/* UHCI TDs hold 32-bit addresses; a buffer that reaches past 4 GB is
 * refused before anything is queued, and the toggle is left alone. */
static void test_buffer_above_4gb_rejected(void)
{
    const usb_msc_t *m = usb_msc_at(0);

    CU_ASSERT_PTR_NOT_NULL(m);
    if (!m) {
        return;
    }
    uint8_t toggle = 1;
    uint32_t actual = 99;
    int rc = usb_bulk(m->dev, m->in_ep, m->in_mps, &toggle,
                      (void *)0x100000000ull, 512, &actual);

    CU_ASSERT_EQUAL(rc, USB_EINVAL);
    CU_ASSERT_EQUAL(toggle, 1);
    CU_ASSERT_EQUAL(actual, 0);

    rc = usb_bulk(m->dev, m->in_ep, m->in_mps, &toggle,
                  (void *)0xFFFFFE00ull, 1024, &actual);   /* straddles 4 GB */
    CU_ASSERT_EQUAL(rc, USB_EINVAL);
}

/* ---- A fake controller --------------------------------------------------- */

typedef enum {
    CSW_GOOD,
    CSW_BAD_SIGNATURE,
    CSW_BAD_TAG,
    CSW_FAILED,         /* status 1 */
    CSW_PHASE_ERROR,    /* status 2 */
    CSW_STALL_ONCE,     /* the first CSW read STALLs, the next is good */
} csw_mode_t;

typedef struct {
    csw_mode_t mode;
    uint32_t   last_tag;
    int        stalled;
    int        resets;          /* class Mass Storage Reset requests */
    int        halts_in;        /* CLEAR_FEATURE(HALT) on the IN endpoint */
    int        halts_out;
    int        bulk_calls;
} fake_t;

static fake_t g_fake;
static usb_hcd_t g_fake_hcd;
static usb_device_t g_fake_dev;
static usb_msc_t g_fake_msc;

#define FAKE_IN_EP  0x81
#define FAKE_OUT_EP 0x02

static int fake_control(usb_hcd_t *hcd, const usb_device_t *dev,
                        const usb_setup_t *setup, void *data, uint16_t *actual)
{
    (void)hcd; (void)dev; (void)data;
    if (actual) {
        *actual = 0;
    }
    if (setup->bRequest == USB_MSC_REQ_RESET) {
        g_fake.resets++;
    } else if (setup->bRequest == USB_REQ_CLEAR_FEATURE) {
        if (setup->wIndex == FAKE_IN_EP) {
            g_fake.halts_in++;
        } else if (setup->wIndex == FAKE_OUT_EP) {
            g_fake.halts_out++;
        }
    }
    return USB_OK;
}

static int fake_bulk(usb_hcd_t *hcd, const usb_device_t *dev, uint8_t ep_addr,
                     uint16_t max_packet, uint8_t *toggle, void *data,
                     uint32_t len, uint32_t *actual)
{
    (void)hcd; (void)dev; (void)max_packet;
    g_fake.bulk_calls++;
    *actual = 0;

    if (ep_addr == FAKE_OUT_EP) {                    /* CBW: remember its tag */
        memcpy(&g_fake.last_tag, (uint8_t *)data + 4, 4);
        *toggle ^= 1;
        *actual = len;
        return USB_OK;
    }

    /* CSW */
    if (g_fake.mode == CSW_STALL_ONCE && !g_fake.stalled) {
        g_fake.stalled = 1;
        return USB_ESTALL;
    }
    uint32_t sig = 0x53425355u;
    uint32_t tag = g_fake.last_tag;
    uint8_t status = 0;

    switch (g_fake.mode) {
    case CSW_BAD_SIGNATURE: sig = 0xDEADBEEFu;   break;
    case CSW_BAD_TAG:       tag ^= 0x100u;       break;
    case CSW_FAILED:        status = 1;          break;
    case CSW_PHASE_ERROR:   status = 2;          break;
    default:                                     break;
    }
    uint8_t csw[13] = { 0 };
    memcpy(csw, &sig, 4);
    memcpy(csw + 4, &tag, 4);
    csw[12] = status;
    memcpy(data, csw, len < sizeof(csw) ? len : sizeof(csw));
    *toggle ^= 1;
    *actual = sizeof(csw);
    return USB_OK;
}

static const usb_hcd_ops_t g_fake_ops = {
    .name    = "fake",
    .control = fake_control,
    .bulk    = fake_bulk,
};

static void fake_setup(csw_mode_t mode)
{
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.mode = mode;
    memset(&g_fake_hcd, 0, sizeof(g_fake_hcd));
    g_fake_hcd.ops = &g_fake_ops;
    memset(&g_fake_dev, 0, sizeof(g_fake_dev));
    g_fake_dev.hcd = &g_fake_hcd;
    g_fake_dev.address = 1;
    memset(&g_fake_msc, 0, sizeof(g_fake_msc));
    g_fake_msc.dev = &g_fake_dev;
    g_fake_msc.in_ep = FAKE_IN_EP;
    g_fake_msc.out_ep = FAKE_OUT_EP;
    g_fake_msc.in_mps = 64;
    g_fake_msc.out_mps = 64;
}

static int fake_tur(void)
{
    uint8_t cdb[6] = { 0 };             /* TEST UNIT READY */

    return usb_msc_command(&g_fake_msc, cdb, sizeof(cdb), 0, NULL, 0);
}

static void test_fake_good_command(void)
{
    fake_setup(CSW_GOOD);
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_OK);
    CU_ASSERT_EQUAL(g_fake.bulk_calls, 2);          /* CBW + CSW, no data */
    CU_ASSERT_EQUAL(g_fake.resets, 0);
}

static void test_fake_failed_status_is_check(void)
{
    fake_setup(CSW_FAILED);
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_CHECK);
    CU_ASSERT_EQUAL(g_fake.resets, 0);              /* a failed command is not a transport fault */
}

static void test_fake_bad_signature_runs_reset_recovery(void)
{
    fake_setup(CSW_BAD_SIGNATURE);
    g_fake_msc.in_toggle = 1;
    g_fake_msc.out_toggle = 1;

    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_EIO);
    CU_ASSERT_EQUAL(g_fake.resets, 1);
    CU_ASSERT_EQUAL(g_fake.halts_in, 1);
    CU_ASSERT_EQUAL(g_fake.halts_out, 1);
    CU_ASSERT_EQUAL(g_fake_msc.in_toggle, 0);
    CU_ASSERT_EQUAL(g_fake_msc.out_toggle, 0);
}

static void test_fake_bad_tag_runs_reset_recovery(void)
{
    fake_setup(CSW_BAD_TAG);
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_EIO);
    CU_ASSERT_EQUAL(g_fake.resets, 1);
}

static void test_fake_phase_error_runs_reset_recovery(void)
{
    fake_setup(CSW_PHASE_ERROR);
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_EIO);
    CU_ASSERT_EQUAL(g_fake.resets, 1);
}

/* A STALL on the status phase is cleared on that endpoint and the CSW read
 * again; no full reset. */
static void test_fake_csw_stall_clears_halt_and_retries(void)
{
    fake_setup(CSW_STALL_ONCE);
    g_fake_msc.in_toggle = 1;

    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_OK);
    CU_ASSERT_EQUAL(g_fake.halts_in, 1);
    CU_ASSERT_EQUAL(g_fake.halts_out, 0);
    CU_ASSERT_EQUAL(g_fake.resets, 0);
    CU_ASSERT_EQUAL(g_fake_msc.in_toggle, 1);       /* 0 after the clear, 1 after the CSW */
}

static void test_fake_tags_increase(void)
{
    fake_setup(CSW_GOOD);
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_OK);
    uint32_t first = g_fake.last_tag;
    CU_ASSERT_EQUAL(fake_tur(), USB_MSC_OK);
    CU_ASSERT_EQUAL(g_fake.last_tag, first + 1);
}

void suite_usb_msc_tests(CU_pSuite s)
{
    CU_add_test(s, "stick claimed by usb_msc and registered as a block device",
                test_stick_claimed_and_registered);
    CU_add_test(s, "known sector reads back (ExoFS superblock)",
                test_known_sector_reads);
    CU_add_test(s, "written sector reads back",
                test_write_then_read_back);
    CU_add_test(s, "128-sector read matches 128 single-sector reads",
                test_large_read_matches_single_sector_reads);
    CU_add_test(s, "read past the end fails, unit still answers",
                test_read_past_end_fails_then_recovers);
    CU_add_test(s, "bulk buffer above 4 GB rejected",
                test_buffer_above_4gb_rejected);
    CU_add_test(s, "fake: good command is CBW + CSW",
                test_fake_good_command);
    CU_add_test(s, "fake: CSW status 1 is CHECK, no reset",
                test_fake_failed_status_is_check);
    CU_add_test(s, "fake: bad CSW signature -> reset recovery",
                test_fake_bad_signature_runs_reset_recovery);
    CU_add_test(s, "fake: CSW tag mismatch -> reset recovery",
                test_fake_bad_tag_runs_reset_recovery);
    CU_add_test(s, "fake: CSW phase error -> reset recovery",
                test_fake_phase_error_runs_reset_recovery);
    CU_add_test(s, "fake: STALL on CSW clears the halt and retries",
                test_fake_csw_stall_clears_halt_and_retries);
    CU_add_test(s, "fake: tags increase per command",
                test_fake_tags_increase);
}
