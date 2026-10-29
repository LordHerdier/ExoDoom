#pragma once
#include <stdint.h>
#include "usb.h"

/*
 * usb_msc — USB mass-storage class driver: Bulk-Only Transport carrying the
 * SCSI transparent command set (SCRUM-247, epic SCRUM-243; design in
 * docs/drivers/usb.md).
 *
 * Claims every interface of class 8 (mass storage), subclass 6 (SCSI
 * transparent command set), protocol 0x50 (Bulk-Only Transport), checks the
 * unit is a direct-access block device with 512-byte sectors, and registers
 * it in the block-device table (src/blkdev.h) in the first free slot from 1
 * up -- so a lone stick is block device 1, and exo_disk_*, ExoFS and the
 * shell's `install` work on it unchanged.
 *
 * Every transfer is synchronous and busy-waited on the controller's frame
 * clock, both at probe (interrupts off, from usb_init()) and later (from a
 * syscall, interrupts on). Nothing here runs from IRQ0 and nothing touches
 * the interrupt pipes usb_poll() services.
 */

#define USB_CLASS_MASS_STORAGE   8
#define USB_MSC_SUBCLASS_SCSI    6
#define USB_MSC_PROTO_BOT        0x50

/* Class requests (USB Mass Storage Bulk-Only Transport 1.0 section 3). */
#define USB_MSC_REQ_RESET        0xFF

#define USB_MSC_MAX              2

/* usb_msc_command() results. Anything below 0 means the transport itself
 * failed and has been through reset recovery. */
#define USB_MSC_OK     0
#define USB_MSC_CHECK  1     /* command failed: CSW status 1, sense follows */
#define USB_MSC_EIO  (-1)

#define USB_MSC_SECTOR 512

/* One Bulk-Only unit. Public so tests can drive the transport over a
 * fake controller; a driver user only ever gets one from usb_msc_at(). */
typedef struct {
    const usb_device_t *dev;
    uint8_t  iface;
    uint8_t  in_ep, out_ep;       /* endpoint addresses, direction bit set on IN */
    uint16_t in_mps, out_mps;
    uint8_t  in_toggle, out_toggle;
    uint32_t tag;
    uint32_t sectors;             /* from READ CAPACITY; 0 until probed */
} usb_msc_t;

/* Class-driver probe, offered every configuration by the USB core. */
int usb_msc_probe(usb_device_t *dev, const uint8_t *config, uint16_t len);

/* Units claimed so far, in claim order. */
int usb_msc_count(void);
const usb_msc_t *usb_msc_at(int idx);

/* Run one command: CBW out, `len` bytes of data in the `dir_in` direction
 * (0 for none), CSW in. Returns USB_MSC_OK, USB_MSC_CHECK, or USB_MSC_EIO. */
int usb_msc_command(usb_msc_t *m, const uint8_t *cdb, uint8_t cdb_len,
                    int dir_in, void *data, uint32_t len);

/* Bulk-Only Mass Storage Reset, then CLEAR_FEATURE(ENDPOINT_HALT) on both
 * bulk endpoints and both data toggles back to DATA0 (BOT section 5.3.4). */
void usb_msc_reset_recovery(usb_msc_t *m);

/* SCSI transfers of `count` sectors (1..128) at `lba`. 0 on success, a
 * negative BLKDEV_E* otherwise. These are the block-device ops. */
int usb_msc_read(void *ctx, uint32_t lba, uint8_t *buf, uint32_t count);
int usb_msc_write(void *ctx, uint32_t lba, const uint8_t *buf, uint32_t count);
