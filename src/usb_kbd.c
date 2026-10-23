/*
 * usb_kbd.c — USB HID boot-protocol keyboard (SCRUM-198). See usb_kbd.h.
 */

#include "usb_kbd.h"
#include "kbd_ring.h"
#include "serial.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

typedef struct {
    usb_device_t *dev;          /* NULL for a test slot */
    uint8_t  iface;
    int      pipe;
    uint8_t  prev[USB_KBD_REPORT_SIZE];
    uint8_t  modifiers;         /* MOD_* mask, from the last report */
    kbd_ring_t ring;
} usb_kbd_t;

static usb_kbd_t g_kbd[USB_KBD_MAX];
static int       g_nkbd;

/* ── Usage translation ────────────────────────────────────────────────── */

ps2_key_t usb_kbd_usage_to_key(uint8_t u)
{
    if (u >= 0x04 && u <= 0x1D) {
        return (ps2_key_t)(KEY_A + (u - 0x04));          /* a..z */
    }
    if (u >= 0x1E && u <= 0x26) {
        return (ps2_key_t)(KEY_1 + (u - 0x1E));          /* 1..9 */
    }
    if (u >= 0x3A && u <= 0x45) {
        return (ps2_key_t)(KEY_F1 + (u - 0x3A));         /* F1..F12 */
    }

    switch (u) {
        case 0x27: return KEY_0;
        case 0x28: return KEY_ENTER;
        case 0x29: return KEY_ESC;
        case 0x2A: return KEY_BACKSPACE;
        case 0x2B: return KEY_TAB;
        case 0x2C: return KEY_SPACE;
        case 0x2D: return KEY_MINUS;
        case 0x2E: return KEY_EQUALS;
        case 0x33: return KEY_SEMICOLON;
        case 0x36: return KEY_COMMA;
        case 0x37: return KEY_PERIOD;
        case 0x38: return KEY_SLASH;
        case 0x4F: return KEY_RIGHT;
        case 0x50: return KEY_LEFT;
        case 0x51: return KEY_DOWN;
        case 0x52: return KEY_UP;
        default:   return KEY_UNKNOWN;
    }
}

/* Boot report modifier byte, bit by bit: LCtrl, LShift, LAlt, LGUI, RCtrl,
 * RShift, RAlt, RGUI. The GUI keys have no ps2_key_t, so no event. */
static const struct {
    ps2_key_t key;
    uint8_t   mod;
} g_mod_bits[8] = {
    { KEY_CTRL,        MOD_LCTRL  },
    { KEY_SHIFT_LEFT,  MOD_LSHIFT },
    { KEY_ALT,         MOD_LALT   },
    { KEY_UNKNOWN,     0          },
    { KEY_CTRL,        MOD_RCTRL  },
    { KEY_SHIFT_RIGHT, MOD_RSHIFT },
    { KEY_ALT,         MOD_RALT   },
    { KEY_UNKNOWN,     0          },
};

/* ── Report decoding ──────────────────────────────────────────────────── */

static void emit(usb_kbd_t *k, ps2_key_t key, int pressed)
{
    if (key == KEY_UNKNOWN) {
        return;
    }
    kbd_event_t ev = {
        .pressed   = pressed ? 1 : 0,
        .key       = (uint8_t)key,
        .modifiers = k->modifiers,
    };
    kbd_ring_push(&k->ring, ev);
}

static int has_usage(const uint8_t *keys, uint8_t u)
{
    for (int i = 0; i < 6; i++) {
        if (keys[i] == u) {
            return 1;
        }
    }
    return 0;
}

static void decode(usb_kbd_t *k, const uint8_t *r, uint32_t len)
{
    uint8_t cur[USB_KBD_REPORT_SIZE] = { 0 };

    if (len < 3) {
        return;
    }
    memcpy(cur, r, len < sizeof(cur) ? len : sizeof(cur));

    /* A rollover report says "too many keys", not "these keys": keep the
     * last good state rather than releasing everything. */
    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        if (cur[i] != 0 && cur[i] <= USB_HID_USAGE_ERR_MAX) {
            return;
        }
    }

    /* Modifiers first, each updating the mask before its own event -- the
     * same order src/ps2.c's update_modifier_state() gives a PS/2 shift. */
    uint8_t changed = (uint8_t)(cur[0] ^ k->prev[0]);
    for (int b = 0; b < 8; b++) {
        if (!(changed & (1u << b)) || g_mod_bits[b].key == KEY_UNKNOWN) {
            continue;
        }
        int down = (cur[0] >> b) & 1;
        k->modifiers = down ? (uint8_t)(k->modifiers | g_mod_bits[b].mod)
                            : (uint8_t)(k->modifiers & ~g_mod_bits[b].mod);
        emit(k, g_mod_bits[b].key, down);
    }

    /* Releases, then presses, so a key swapped for another in one report
     * reads as up-then-down. */
    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        uint8_t u = k->prev[i];
        if (u != 0 && !has_usage(&cur[2], u)) {
            emit(k, usb_kbd_usage_to_key(u), 0);
        }
    }
    for (int i = 2; i < USB_KBD_REPORT_SIZE; i++) {
        uint8_t u = cur[i];
        if (u != 0 && !has_usage(&k->prev[2], u)) {
            emit(k, usb_kbd_usage_to_key(u), 1);
        }
    }

    memcpy(k->prev, cur, sizeof(cur));
}

/* Interrupt-IN callback: runs in irq0_handler() via usb_poll(). */
static void report_cb(void *ctx, const uint8_t *data, uint32_t len)
{
    decode((usb_kbd_t *)ctx, data, len);
}

void usb_kbd_process_report(int idx, const uint8_t *report, uint32_t len)
{
    if (idx < 0 || idx >= g_nkbd || report == NULL) {
        return;
    }
    decode(&g_kbd[idx], report, len);
}

/* ── Probe ────────────────────────────────────────────────────────────── */

static void slot_init(usb_kbd_t *k)
{
    memset(k, 0, sizeof(*k));
    k->pipe = -1;
    kbd_ring_init(&k->ring);
}

void usb_kbd_reset(void)
{
    g_nkbd = 0;
}

static void log_fail(const char *what, int rc)
{
    serial_print("usb_kbd: ");
    serial_print(what);
    serial_print(" failed (rc=");
    serial_print_hex64((uint64_t)(int64_t)rc);
    serial_print(") -- ");
}

int usb_kbd_probe(usb_device_t *dev, const uint8_t *config, uint16_t len)
{
    const usb_interface_desc_t *iface;
    const usb_endpoint_desc_t *ep;

    if (usb_config_find_interface(config, len, USB_CLASS_HID,
                                  USB_HID_SUBCLASS_BOOT,
                                  USB_HID_PROTO_KEYBOARD, &iface, &ep)
        != USB_OK || ep == NULL) {
        return 0;
    }
    if (g_nkbd >= USB_KBD_MAX) {
        serial_print("usb_kbd: table full -- ");
        return 0;
    }

    uint16_t mps = ep->wMaxPacketSize & 0x7FF;
    if (mps < 3) {
        return 0;
    }
    if (mps > USB_KBD_REPORT_SIZE) {
        mps = USB_KBD_REPORT_SIZE;    /* boot reports are 8 bytes */
    }

    int rc = usb_set_configuration(dev, dev->config_value);
    if (rc != USB_OK) {
        log_fail("SET_CONFIGURATION", rc);
        return 0;
    }

    uint8_t rt = USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE;
    rc = usb_control(dev, rt, USB_HID_REQ_SET_PROTOCOL, USB_HID_PROTOCOL_BOOT,
                     iface->bInterfaceNumber, NULL, 0, NULL);
    if (rc != USB_OK) {
        log_fail("SET_PROTOCOL(boot)", rc);
        return 0;
    }
    /* Idle 0: report only on change. Optional for a boot keyboard, so a
     * STALL here is not fatal (HID 1.11 §7.2.4). */
    usb_control(dev, rt, USB_HID_REQ_SET_IDLE, 0, iface->bInterfaceNumber,
                NULL, 0, NULL);

    usb_kbd_t *k = &g_kbd[g_nkbd];
    slot_init(k);
    k->dev = dev;
    k->iface = iface->bInterfaceNumber;

    int pipe = dev->hcd->ops->interrupt_open(dev->hcd, dev,
                                             ep->bEndpointAddress
                                                 & USB_EP_NUM_MASK,
                                             mps, report_cb, k);
    if (pipe < 0) {
        log_fail("interrupt_open", pipe);
        return 0;
    }
    k->pipe = pipe;
    g_nkbd++;
    return 1;
}

/* ── Consumer API ─────────────────────────────────────────────────────── */

int usb_kbd_count(void)
{
    return g_nkbd;
}

int usb_kbd_pop(int idx, kbd_event_t *out)
{
    if (idx < 0 || idx >= g_nkbd || out == NULL) {
        return 0;
    }
    return kbd_ring_pop(&g_kbd[idx].ring, out);
}

uint32_t usb_kbd_dropped(int idx)
{
    if (idx < 0 || idx >= g_nkbd) {
        return 0;
    }
    return kbd_ring_dropped(&g_kbd[idx].ring);
}

int usb_kbd_ident(int idx, usb_kbd_ident_t *out)
{
    if (idx < 0 || idx >= g_nkbd || out == NULL || g_kbd[idx].dev == NULL) {
        return USB_EINVAL;
    }
    const usb_device_t *d = g_kbd[idx].dev;
    out->hcd = d->hcd->index;
    out->port = d->port;
    out->address = d->address;
    out->pipe = g_kbd[idx].pipe;
    return USB_OK;
}

#ifdef TESTING
int usb_kbd_test_attach(void)
{
    if (g_nkbd >= USB_KBD_MAX) {
        return -1;
    }
    slot_init(&g_kbd[g_nkbd]);
    return g_nkbd++;
}

void usb_kbd_test_detach(int idx)
{
    if (idx == g_nkbd - 1 && g_kbd[idx].dev == NULL) {
        g_nkbd--;
    }
}
#endif
