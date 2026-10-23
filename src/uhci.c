/*
 * uhci.c — UHCI host controller driver (SCRUM-198).
 *
 * See uhci.h for the contract and docs/drivers/usb.md sections 3-8 for why
 * it is shaped this way. In brief:
 *
 *   - Every controller is taken from the BIOS (LEGSUP, GRESET, HCRESET)
 *     before anything else, because SeaBIOS leaves them running on its own
 *     schedule.
 *   - The schedule is fixed: all 1024 frame list entries point at one
 *     interrupt skeleton QH. Interrupt-IN pipes are QHs pushed onto the
 *     front of its horizontal chain, which ends in the control QH. So every
 *     pipe is visited every frame; a HID device NAKs until it has something
 *     new, so that costs nothing but the controller's own walk.
 *   - USBINTR stays 0. Control transfers busy-wait on their TDs (they only
 *     run from usb_init(), with interrupts off); interrupt pipes are checked
 *     and re-armed from irq0_handler() via usb_poll().
 *
 * Every structure the controller reads or writes comes from alloc_page(),
 * which on the kernel's identity map means the pointer is the physical
 * address. UHCI addresses are 32-bit, so each page is checked to lie below
 * 4 GB when it is allocated.
 */

#include "uhci.h"
#include "usb.h"
#include "pci.h"
#include "page_alloc.h"
#include "serial.h"
#include "string.h"
#include "io.h"

#include <stdint.h>
#include <stddef.h>

/* ── Transfer descriptors and queue heads (UHCI 1.1 §3.2-3.3) ─────────── */

typedef struct {
    volatile uint32_t link;
    volatile uint32_t ctrl;     /* status + control */
    volatile uint32_t token;
    volatile uint32_t buffer;
    uint32_t sw[4];             /* pads to 32 bytes; hardware ignores it */
} uhci_td_t;

typedef struct {
    volatile uint32_t head;     /* horizontal: next QH */
    volatile uint32_t element;  /* vertical: first TD still to run */
    uint32_t sw[2];
} uhci_qh_t;

#define TD_ACTLEN_MASK 0x7FFu
#define TD_BITSTUFF    (1u << 17)
#define TD_CRC_TIMEOUT (1u << 18)
#define TD_NAK         (1u << 19)
#define TD_BABBLE      (1u << 20)
#define TD_DBE         (1u << 21)
#define TD_STALLED     (1u << 22)
#define TD_ACTIVE      (1u << 23)
#define TD_IOC         (1u << 24)
#define TD_LS          (1u << 26)
#define TD_CERR3       (3u << 27)
#define TD_SPD         (1u << 29)
#define TD_ERRORS      (TD_STALLED | TD_DBE | TD_BABBLE | TD_CRC_TIMEOUT \
                        | TD_BITSTUFF)

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

/* Control page layout: TDs from offset 0, setup packet + data from
 * CTRL_BUF_OFF. 62 TDs is a setup, a status and 60 data packets -- 480
 * bytes at the smallest (8-byte) endpoint 0, comfortably above
 * USB_CONFIG_MAX. */
#define CTRL_MAX_TDS   62
#define CTRL_BUF_OFF   2048
#define CTRL_DATA_OFF  (CTRL_BUF_OFF + 16)
#define CTRL_DATA_MAX  (4096 - CTRL_DATA_OFF)

/* Sched page layout: interrupt skeleton QH at 0, control QH at 16, then
 * one 128-byte slot per interrupt pipe: QH, TD, packet buffer. */
#define SCHED_PIPE_OFF  64
#define SCHED_PIPE_SIZE 128
#define PIPE_TD_OFF     32
#define PIPE_BUF_OFF    64
#define PIPE_BUF_MAX    64

/* How long a control transfer may take, in 1 ms frames, and an absolute
 * cap on busy-wait iterations in case FRNUM itself stops moving. */
#define CTRL_TIMEOUT_FRAMES 500
#define SPIN_CAP            5000000u

typedef struct {
    uhci_qh_t *qh;
    uhci_td_t *td;
    uint8_t   *buf;
    uint8_t    addr, ep, low_speed, toggle, dead;
    uint16_t   max_packet;
    usb_intr_cb_t cb;
    void      *ctx;
} uhci_pipe_t;

typedef struct {
    usb_hcd_t  hcd;
    uint8_t    bus, device, function;
    uint16_t   io;
    int        ports;
    uint32_t  *frame_list;
    uint8_t   *sched;
    uint8_t   *ctrl;
    uhci_qh_t *int_qh;
    uhci_qh_t *ctrl_qh;
    uhci_pipe_t pipes[UHCI_MAX_PIPES];
    volatile int npipes;
    volatile uint32_t pipe_errors;
} uhci_t;

static uhci_t g_hc[UHCI_MAX_CONTROLLERS];
static int    g_nhc;

static inline uint32_t phys32(const volatile void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static inline void barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

/* ~1 us per iteration on real hardware (the port-0x80 trick src/hda.c and
 * src/pic.c use). Only for the resets before the controller runs; once it
 * runs, wait_frames() measures time with the controller's own clock. */
static void delay_us(uint32_t us)
{
    while (us--) {
        io_wait();
    }
}

static uint16_t frnum(const uhci_t *hc)
{
    return inw(hc->io + UHCI_FRNUM) & 0x7FF;
}

/* Wait `n` 1 ms frames by FRNUM, which the controller advances itself. That
 * is the clock that matters for everything the USB spec times, and it works
 * with interrupts off, where kernel_sleep_ms() would hlt forever. */
static void wait_frames(const uhci_t *hc, uint32_t n)
{
    uint16_t start = frnum(hc);
    uint32_t spins = 0;

    while (((frnum(hc) - start) & 0x7FF) < n && spins++ < SPIN_CAP) {
        io_wait();
    }
}

/* A zeroed kernel page below 4 GB, or NULL. */
static void *alloc_dma_page(void)
{
    void *p = alloc_page();

    if (p == NULL) {
        return NULL;
    }
    if ((uint64_t)(uintptr_t)p >> 32) {
        free_page(p);
        return NULL;
    }
    memset(p, 0, 4096);
    return p;
}

static uint32_t td_token(uint8_t pid, uint8_t addr, uint8_t ep,
                         uint8_t toggle, uint16_t len)
{
    /* MaxLen is encoded n-1, with 0x7FF meaning a zero-length packet. */
    uint32_t maxlen = len ? (uint32_t)(len - 1) : 0x7FFu;

    return (uint32_t)pid | ((uint32_t)addr << 8) | ((uint32_t)ep << 15)
         | ((uint32_t)toggle << 19) | (maxlen << 21);
}

static uint32_t td_actlen(uint32_t ctrl)
{
    return (ctrl + 1) & TD_ACTLEN_MASK;
}

static uint32_t td_maxlen(uint32_t token)
{
    return ((token >> 21) + 1) & TD_ACTLEN_MASK;
}

static int td_error(uint32_t ctrl)
{
    return (ctrl & TD_STALLED) ? USB_ESTALL : USB_EIO;
}

/* ── Root ports ───────────────────────────────────────────────────────── */

/* PORTSC write that leaves the two write-1-to-clear change bits alone
 * unless `set` names them. */
static void port_write(const uhci_t *hc, int port, uint16_t clear,
                       uint16_t set)
{
    uint16_t reg = UHCI_PORTSC(port);
    uint16_t v = inw(hc->io + reg);

    v &= (uint16_t)~(UHCI_PORT_CSC | UHCI_PORT_PEC | clear);
    outw(hc->io + reg, (uint16_t)(v | set));
}

/* No register says how many ports there are. A real one reads bit 7 as 1;
 * stop at the first that doesn't, or that reads all ones (docs/drivers/
 * usb.md section 4). */
static int count_ports(const uhci_t *hc)
{
    int n = 0;

    while (n < UHCI_MAX_PORTS) {
        uint16_t v = inw(hc->io + UHCI_PORTSC(n));
        if (!(v & UHCI_PORT_RSVD1) || v == 0xFFFF) {
            break;
        }
        n++;
    }
    return n;
}

static int uhci_port_count(usb_hcd_t *hcd)
{
    return ((uhci_t *)hcd->priv)->ports;
}

static int uhci_port_reset(usb_hcd_t *hcd, int port, uint8_t *speed)
{
    uhci_t *hc = hcd->priv;

    if (port < 0 || port >= hc->ports) {
        return USB_EINVAL;
    }
    if (!(inw(hc->io + UHCI_PORTSC(port)) & UHCI_PORT_CCS)) {
        return USB_ENODEV;
    }

    /* USB 2.0 §7.1.7.5: drive reset for at least 50 ms on a root port. */
    port_write(hc, port, 0, UHCI_PORT_PR);
    wait_frames(hc, 50);
    port_write(hc, port, UHCI_PORT_PR, 0);
    delay_us(50);

    /* Enable can take a few tries while the port leaves reset (Linux
     * uhci_hub does the same loop). */
    for (int tries = 0; tries < 10; tries++) {
        uint16_t v = inw(hc->io + UHCI_PORTSC(port));
        if (!(v & UHCI_PORT_CCS)) {
            return USB_ENODEV;
        }
        if (v & UHCI_PORT_PE) {
            break;
        }
        port_write(hc, port, 0,
                   UHCI_PORT_CSC | UHCI_PORT_PEC | UHCI_PORT_PE);
        wait_frames(hc, 1);
    }

    uint16_t v = inw(hc->io + UHCI_PORTSC(port));
    if (!(v & UHCI_PORT_PE)) {
        return USB_EIO;
    }
    port_write(hc, port, 0, UHCI_PORT_CSC | UHCI_PORT_PEC);

    /* TRSTRCY: 10 ms before the device must answer SET_ADDRESS. */
    wait_frames(hc, 10);

    if (speed) {
        *speed = (v & UHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
    }
    return USB_OK;
}

/* ── Control transfers ────────────────────────────────────────────────── */

static int uhci_control(usb_hcd_t *hcd, const usb_device_t *dev,
                        const usb_setup_t *setup, void *data,
                        uint16_t *actual)
{
    uhci_t *hc = hcd->priv;
    uint16_t len = setup->wLength;
    int in = (setup->bmRequestType & USB_RT_IN) != 0;
    uint16_t mps = dev->max_packet0 ? dev->max_packet0 : 8;
    uint32_t ls = (dev->speed == USB_SPEED_LOW) ? TD_LS : 0;
    uint32_t base_ctrl = TD_ACTIVE | TD_CERR3 | ls;
    uhci_td_t *td = (uhci_td_t *)hc->ctrl;
    uint8_t *setup_buf = hc->ctrl + CTRL_BUF_OFF;
    uint8_t *data_buf = hc->ctrl + CTRL_DATA_OFF;
    int ntd = 2 + (len + mps - 1) / mps;
    int rc = USB_OK;
    uint32_t got = 0;

    if (actual) {
        *actual = 0;
    }
    if (len > CTRL_DATA_MAX || ntd > CTRL_MAX_TDS || (len && data == NULL)) {
        return USB_EINVAL;
    }

    memcpy(setup_buf, setup, sizeof(*setup));
    if (!in && len) {
        memcpy(data_buf, data, len);
    }

    /* SETUP is always DATA0; the data stage starts at DATA1 and alternates;
     * the status stage is DATA1 in the opposite direction. Data IN packets
     * carry SPD so a short packet halts the queue instead of running the
     * next IN TD against a device that has already moved to status. */
    td[0].ctrl   = base_ctrl;
    td[0].token  = td_token(PID_SETUP, dev->address, 0, 0, 8);
    td[0].buffer = phys32(setup_buf);

    uint8_t toggle = 1;
    int k = 1;
    for (uint16_t off = 0; off < len; off += mps, k++) {
        uint16_t n = (uint16_t)((len - off) < mps ? (len - off) : mps);
        td[k].ctrl   = base_ctrl | (in ? TD_SPD : 0);
        td[k].token  = td_token(in ? PID_IN : PID_OUT, dev->address, 0,
                                toggle, n);
        td[k].buffer = phys32(data_buf + off);
        toggle ^= 1;
    }

    td[k].ctrl   = base_ctrl;
    td[k].token  = td_token(in ? PID_OUT : PID_IN, dev->address, 0, 1, 0);
    td[k].buffer = 0;

    for (int i = 0; i < ntd; i++) {
        td[i].link = (i + 1 < ntd) ? (phys32(&td[i + 1]) | UHCI_LP_VF)
                                   : UHCI_LP_TERM;
    }

    barrier();
    hc->ctrl_qh->element = phys32(&td[0]);

    /* Walk the chain in order as the controller finishes it. */
    uint16_t start = frnum(hc);
    uint32_t spins = 0;
    int i = 0;
    while (i < ntd) {
        uint32_t s = td[i].ctrl;

        if (s & TD_ACTIVE) {
            if (((frnum(hc) - start) & 0x7FF) >= CTRL_TIMEOUT_FRAMES
                || spins++ >= SPIN_CAP) {
                rc = USB_ETIMEOUT;
                break;
            }
            io_wait();
            continue;
        }
        if (s & TD_ERRORS) {
            rc = td_error(s);
            break;
        }
        if (i > 0 && i < ntd - 1) {
            uint32_t n = td_actlen(s);
            got += n;
            if (in && n < td_maxlen(td[i].token)) {
                /* Short packet: SPD left the QH parked on this TD. Restart
                 * it at the status stage. */
                i = ntd - 1;
                hc->ctrl_qh->element = phys32(&td[i]);
                continue;
            }
        }
        i++;
    }

    /* Unhook the chain whatever happened, and let the controller finish any
     * frame that might still be looking at it before the TDs are reused. */
    hc->ctrl_qh->element = UHCI_LP_TERM;
    wait_frames(hc, 1);

    if (rc == USB_OK) {
        if (in && got) {
            memcpy(data, data_buf, got);
        }
        if (actual) {
            *actual = (uint16_t)got;
        }
    }
    return rc;
}

/* ── Interrupt pipes ──────────────────────────────────────────────────── */

static void pipe_arm(uhci_pipe_t *p)
{
    p->td->token = td_token(PID_IN, p->addr, p->ep, p->toggle,
                            p->max_packet);
    p->td->ctrl = TD_ACTIVE | TD_CERR3 | (p->low_speed ? TD_LS : 0);
    barrier();
    p->qh->element = phys32(p->td);
}

static int uhci_interrupt_open(usb_hcd_t *hcd, const usb_device_t *dev,
                               uint8_t ep, uint16_t max_packet,
                               usb_intr_cb_t cb, void *ctx)
{
    uhci_t *hc = hcd->priv;
    int idx = hc->npipes;

    if (cb == NULL || max_packet == 0 || max_packet > PIPE_BUF_MAX
        || ep > USB_EP_NUM_MASK) {
        return USB_EINVAL;
    }
    if (idx >= UHCI_MAX_PIPES) {
        return USB_ENOMEM;
    }

    uint8_t *slot = hc->sched + SCHED_PIPE_OFF + idx * SCHED_PIPE_SIZE;
    uhci_pipe_t *p = &hc->pipes[idx];

    p->qh = (uhci_qh_t *)slot;
    p->td = (uhci_td_t *)(slot + PIPE_TD_OFF);
    p->buf = slot + PIPE_BUF_OFF;
    p->addr = dev->address;
    p->ep = ep;
    p->low_speed = dev->speed == USB_SPEED_LOW;
    p->toggle = 0;
    p->dead = 0;
    p->max_packet = max_packet;
    p->cb = cb;
    p->ctx = ctx;

    p->td->link = UHCI_LP_TERM;
    p->td->buffer = phys32(p->buf);
    p->qh->element = UHCI_LP_TERM;
    pipe_arm(p);

    /* Push onto the front of the interrupt skeleton's chain. The QH is
     * complete before the skeleton points at it, so a frame in progress
     * sees either the old chain or the whole new one. */
    p->qh->head = hc->int_qh->head;
    barrier();
    hc->int_qh->head = phys32(p->qh) | UHCI_LP_QH;
    barrier();
    hc->npipes = idx + 1;
    return idx;
}

static int uhci_bulk(usb_hcd_t *hcd, const usb_device_t *dev,
                     uint8_t ep_addr, void *data, uint32_t len,
                     uint32_t *actual)
{
    (void)hcd; (void)dev; (void)ep_addr; (void)data; (void)len;
    if (actual) {
        *actual = 0;
    }
    return USB_ENOTSUP;   /* SCRUM-247 */
}

/*
 * IRQ0 service, inside the budget docs/drivers/usb.md section 3 sets: read
 * USBSTS, read each pipe's TD status, hand a finished packet (<= 64 bytes,
 * 8 for a boot keyboard) to its callback, and re-arm the TD (token, status,
 * QH element). No schedule walk, no control transfer, no serial.
 */
static void uhci_poll(usb_hcd_t *hcd)
{
    uhci_t *hc = hcd->priv;
    uint16_t sts = inw(hc->io + UHCI_USBSTS);

    if (sts & (UHCI_STS_USBINT | UHCI_STS_ERRINT)) {
        outw(hc->io + UHCI_USBSTS,
             (uint16_t)(sts & (UHCI_STS_USBINT | UHCI_STS_ERRINT)));
    }

    int n = hc->npipes;
    for (int i = 0; i < n; i++) {
        uhci_pipe_t *p = &hc->pipes[i];
        if (p->dead) {
            continue;
        }

        uint32_t s = p->td->ctrl;
        if (s & TD_ACTIVE) {
            continue;
        }
        if (s & TD_ERRORS) {
            hc->pipe_errors++;
            if (s & TD_STALLED) {
                /* A halted endpoint needs CLEAR_FEATURE, which is a control
                 * transfer and not allowed here. Leave it parked. */
                p->dead = 1;
                continue;
            }
            pipe_arm(p);          /* same toggle: the packet never landed */
            continue;
        }

        p->cb(p->ctx, p->buf, td_actlen(s));
        p->toggle ^= 1;
        pipe_arm(p);
    }
}

static void uhci_delay_ms(usb_hcd_t *hcd, uint32_t ms)
{
    wait_frames(hcd->priv, ms);
}

static const usb_hcd_ops_t uhci_ops = {
    .name           = "uhci",
    .port_count     = uhci_port_count,
    .port_reset     = uhci_port_reset,
    .control        = uhci_control,
    .interrupt_open = uhci_interrupt_open,
    .bulk           = uhci_bulk,
    .poll           = uhci_poll,
    .delay_ms       = uhci_delay_ms,
};

/* ── Bring-up ─────────────────────────────────────────────────────────── */

static int wait_cmd_clear(const uhci_t *hc, uint16_t bit)
{
    for (uint32_t i = 0; i < 100000; i++) {
        if (!(inw(hc->io + UHCI_USBCMD) & bit)) {
            return 1;
        }
        io_wait();
    }
    return 0;
}

static int wait_halted(const uhci_t *hc, int want_halted)
{
    for (uint32_t i = 0; i < 100000; i++) {
        int halted = (inw(hc->io + UHCI_USBSTS) & UHCI_STS_HCH) != 0;
        if (halted == want_halted) {
            return 1;
        }
        io_wait();
    }
    return 0;
}

static void log_hc(const uhci_t *hc, const char *msg)
{
    serial_print("uhci: ");
    serial_print_dec(hc->bus);
    serial_print(":");
    serial_print_dec(hc->device);
    serial_print(".");
    serial_print_dec(hc->function);
    serial_print(" ");
    serial_print(msg);
}

static int uhci_start(uhci_t *hc, const pci_device_t *d)
{
    pci_bar_t bar;

    if (pci_bar_read(d, UHCI_BAR_INDEX, &bar) != PCI_OK || !bar.is_io
        || bar.base == 0) {
        log_hc(hc, "no I/O BAR 4\n");
        return USB_ENODEV;
    }
    hc->io = (uint16_t)bar.base;

    /* Take it from the BIOS: legacy emulation and SMI off, stop the
     * controller and its interrupts, then reset the bus and the
     * controller. SeaBIOS left it running (docs/drivers/usb.md section 6). */
    pci_config_write16(hc->bus, hc->device, hc->function, UHCI_PCI_LEGSUP,
                       UHCI_LEGSUP_TAKEOVER);
    outw(hc->io + UHCI_USBINTR, 0);
    outw(hc->io + UHCI_USBCMD, 0);
    wait_halted(hc, 1);

    outw(hc->io + UHCI_USBCMD, UHCI_CMD_GRESET);
    delay_us(50000);
    outw(hc->io + UHCI_USBCMD, 0);
    delay_us(10000);

    outw(hc->io + UHCI_USBCMD, UHCI_CMD_HCRESET);
    if (!wait_cmd_clear(hc, UHCI_CMD_HCRESET)) {
        log_hc(hc, "HCRESET did not complete\n");
        return USB_EIO;
    }
    outw(hc->io + UHCI_USBINTR, 0);

    if (pci_enable_device(d) != PCI_OK) {
        return USB_ENODEV;
    }

    hc->frame_list = alloc_dma_page();
    hc->sched = alloc_dma_page();
    hc->ctrl = alloc_dma_page();
    if (!hc->frame_list || !hc->sched || !hc->ctrl) {
        log_hc(hc, "no DMA page below 4 GB\n");
        return USB_ENOMEM;
    }

    hc->int_qh = (uhci_qh_t *)hc->sched;
    hc->ctrl_qh = (uhci_qh_t *)(hc->sched + 16);
    hc->ctrl_qh->head = UHCI_LP_TERM;
    hc->ctrl_qh->element = UHCI_LP_TERM;
    hc->int_qh->head = phys32(hc->ctrl_qh) | UHCI_LP_QH;
    hc->int_qh->element = UHCI_LP_TERM;
    for (int f = 0; f < UHCI_FRAMES; f++) {
        hc->frame_list[f] = phys32(hc->int_qh) | UHCI_LP_QH;
    }

    outl(hc->io + UHCI_FRBASEADD, phys32(hc->frame_list));
    outw(hc->io + UHCI_FRNUM, 0);
    outb(hc->io + UHCI_SOFMOD, 64);          /* the 1 ms default */
    outw(hc->io + UHCI_USBSTS, UHCI_STS_ALL);
    outw(hc->io + UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    if (!wait_halted(hc, 0)) {
        log_hc(hc, "did not leave halt\n");
        return USB_EIO;
    }

    hc->ports = count_ports(hc);
    log_hc(hc, "io 0x");
    serial_print_hex(hc->io);
    serial_print(" ports ");
    serial_print_dec((uint32_t)hc->ports);
    serial_print(" frame list 0x");
    serial_print_hex(phys32(hc->frame_list));
    serial_print("\n");
    return USB_OK;
}

int uhci_probe_all(void)
{
    g_nhc = 0;
    for (int i = 0; i < pci_device_count() && g_nhc < UHCI_MAX_CONTROLLERS;
         i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d->class_code != PCI_CLASS_SERIAL_BUS
            || d->subclass != PCI_SUBCLASS_USB
            || d->prog_if != PCI_PROGIF_UHCI) {
            continue;
        }

        uhci_t *hc = &g_hc[g_nhc];
        memset(hc, 0, sizeof(*hc));
        hc->bus = d->bus;
        hc->device = d->device;
        hc->function = d->function;
        hc->hcd.ops = &uhci_ops;
        hc->hcd.priv = hc;

        if (uhci_start(hc, d) != USB_OK) {
            continue;
        }
        if (usb_hcd_register(&hc->hcd) != USB_OK) {
            log_hc(hc, "USB core has no slot left\n");
            outw(hc->io + UHCI_USBCMD, 0);
            break;
        }
        g_nhc++;
    }
    return g_nhc;
}

/* ── Introspection ────────────────────────────────────────────────────── */

int uhci_count(void)
{
    return g_nhc;
}

int uhci_get_info(int idx, uhci_info_t *out)
{
    if (idx < 0 || idx >= g_nhc || out == NULL) {
        return USB_EINVAL;
    }
    const uhci_t *hc = &g_hc[idx];
    out->bus = hc->bus;
    out->device = hc->device;
    out->function = hc->function;
    out->io_base = hc->io;
    out->ports = hc->ports;
    out->frame_list = (uint64_t)(uintptr_t)hc->frame_list;
    out->sched_page = (uint64_t)(uintptr_t)hc->sched;
    out->ctrl_page = (uint64_t)(uintptr_t)hc->ctrl;
    out->pipes = hc->npipes;
    out->pipe_errors = hc->pipe_errors;
    return USB_OK;
}

int uhci_pipe_live(int idx, int pipe)
{
    if (idx < 0 || idx >= g_nhc || pipe < 0 || pipe >= g_hc[idx].npipes) {
        return -1;
    }
    return !g_hc[idx].pipes[pipe].dead;
}
