#pragma once
#include <stdint.h>
#include "ps2.h"
#include "usb.h"

/*
 * usb_kbd — USB HID boot-protocol keyboard class driver (SCRUM-198).
 *
 * Claims every interface that is HID (class 3), boot subclass (1), keyboard
 * protocol (1), switches it to the boot protocol, and polls its interrupt-IN
 * endpoint for the fixed 8-byte boot report: a modifier byte, a reserved
 * byte, then up to six usage IDs of keys currently held.
 *
 * Each keyboard gets its own kbd_ring_t of kbd_event_t -- the same event
 * type, key enum and MOD_* bits as the PS/2 path (src/ps2.h), so a consumer
 * can treat either source alike -- and nothing here touches the PS/2 ring.
 * Which keyboard an event came from is the ring it is on; usb_kbd_ident()
 * says which physical device that is (controller, root port, USB address).
 * Routing a keyboard to a context is SCRUM-199, not this file.
 *
 * Producer: the report callback, from usb_poll() in irq0_handler(). Consumer:
 * usb_kbd_pop(), from ordinary kernel code. Single-producer/single-consumer
 * per ring, as src/kbd_ring.h requires.
 */

#define USB_KBD_MAX         4
#define USB_KBD_REPORT_SIZE 8
/* Largest interrupt packet accepted: full-speed's maximum, and what a UHCI
 * pipe buffer holds. */
#define USB_KBD_MAX_PACKET  64

/* HID class requests (HID 1.11 §7.2). */
#define USB_HID_REQ_SET_IDLE     0x0A
#define USB_HID_REQ_SET_PROTOCOL 0x0B
#define USB_HID_PROTOCOL_BOOT    0

#define USB_CLASS_HID          3
#define USB_HID_SUBCLASS_BOOT  1
#define USB_HID_PROTO_KEYBOARD 1

/* Report usages 0x01-0x03 are error codes (ErrorRollOver, POSTFail,
 * ErrorUndefined), sent in every key slot when too many keys are down. */
#define USB_HID_USAGE_ERR_MAX  0x03

typedef struct {
    int     hcd;       /* usb_hcd_t index           */
    uint8_t port;      /* 0-based root port         */
    uint8_t address;   /* USB device address        */
    int     pipe;      /* controller's pipe index   */
} usb_kbd_ident_t;

/* Forget every keyboard. Called by usb_init() before enumerating. */
void usb_kbd_reset(void);

/* Class-driver probe (usb.c's driver table). Returns 1 if it claimed dev. */
int usb_kbd_probe(usb_device_t *dev, const uint8_t *config, uint16_t len);

int usb_kbd_count(void);
/* Oldest event from keyboard idx: 1 and *out filled, or 0 if none. */
int usb_kbd_pop(int idx, kbd_event_t *out);
uint32_t usb_kbd_dropped(int idx);
/* 1 while keyboard idx's interrupt pipe is still delivering reports (not
 * stalled, unplugged or on a halted controller), 0 otherwise. */
int usb_kbd_live(int idx);
int usb_kbd_ident(int idx, usb_kbd_ident_t *out);

/* Decode one boot report for keyboard idx: diff it against the previous
 * one and queue a press/release per change. The interrupt callback's body;
 * exposed so tests can drive the decoder without hardware. */
void usb_kbd_process_report(int idx, const uint8_t *report, uint32_t len);

/* HID keyboard usage (HID Usage Tables §10) -> ps2_key_t, KEY_UNKNOWN for
 * anything the enum has no key for. */
ps2_key_t usb_kbd_usage_to_key(uint8_t usage);

#ifdef TESTING
/* A keyboard slot with no device behind it, for decoder tests. Returns its
 * index, or -1 if the table is full. Detach must be the most recent attach. */
int usb_kbd_test_attach(void);
void usb_kbd_test_detach(int idx);
#endif
