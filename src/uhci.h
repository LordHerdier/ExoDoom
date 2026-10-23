#pragma once
#include <stdint.h>

/*
 * uhci — Universal Host Controller Interface driver (SCRUM-198).
 *
 * One usb_hcd_t (src/usb.h) per UHCI function PCI reports. Ring 0 only.
 * The design decisions -- polled from IRQ0 with USBINTR = 0, every
 * controller driven, no hub, firmware takeover first -- are SCRUM-245's and
 * are argued in docs/drivers/usb.md sections 3, 4 and 6; the schedule this
 * file builds is described in section 8.
 */

/* I/O registers, offsets from USBBASE (BAR 4). */
#define UHCI_USBCMD    0x00   /* 16-bit */
#define UHCI_USBSTS    0x02   /* 16-bit, write-1-to-clear */
#define UHCI_USBINTR   0x04   /* 16-bit */
#define UHCI_FRNUM     0x06   /* 16-bit, bits 10:0 */
#define UHCI_FRBASEADD 0x08   /* 32-bit, 4 KiB aligned */
#define UHCI_SOFMOD    0x0C   /* 8-bit */
#define UHCI_PORTSC(n) (0x10 + 2 * (n))

#define UHCI_CMD_RS      (1u << 0)
#define UHCI_CMD_HCRESET (1u << 1)
#define UHCI_CMD_GRESET  (1u << 2)
#define UHCI_CMD_CF      (1u << 6)
#define UHCI_CMD_MAXP    (1u << 7)

#define UHCI_STS_USBINT  (1u << 0)
#define UHCI_STS_ERRINT  (1u << 1)
#define UHCI_STS_HSE     (1u << 3)
#define UHCI_STS_HCPE    (1u << 4)
#define UHCI_STS_HCH     (1u << 5)
#define UHCI_STS_ALL     0x003Fu

#define UHCI_PORT_CCS    (1u << 0)
#define UHCI_PORT_CSC    (1u << 1)   /* write-1-to-clear */
#define UHCI_PORT_PE     (1u << 2)
#define UHCI_PORT_PEC    (1u << 3)   /* write-1-to-clear */
#define UHCI_PORT_RSVD1  (1u << 7)   /* always reads 1 on a real port */
#define UHCI_PORT_LSDA   (1u << 8)
#define UHCI_PORT_PR     (1u << 9)

/* PCI config: legacy support register. Writing 0x8F00 clears every
 * trap/status bit and turns off SMI and PIRQ routing (Linux's
 * USBLEGSUP_RWC), which is how the kernel takes the controller from the
 * BIOS's USB legacy emulation. */
#define UHCI_PCI_LEGSUP        0xC0
#define UHCI_LEGSUP_TAKEOVER   0x8F00

#define UHCI_BAR_INDEX 4
#define UHCI_FRAMES    1024
#define UHCI_MAX_PORTS 7      /* Linux uhci_count_ports() cap */
#define UHCI_MAX_CONTROLLERS 8
#define UHCI_MAX_PIPES 8      /* interrupt-IN pipes per controller */

/* Link pointers (frame list entries, QH head/element, TD link). */
#define UHCI_LP_TERM 0x1u
#define UHCI_LP_QH   0x2u
#define UHCI_LP_VF   0x4u     /* TD link only: depth first */

/* Find every UHCI function (class 0x0C/0x03, prog-if 0x00), take it from
 * the firmware, reset it, build a schedule, start it, and register it with
 * the USB core. Returns how many controllers were registered. */
int uhci_probe_all(void);

/* Introspection for tests and usb_dump(). */
typedef struct {
    uint8_t  bus, device, function;
    uint16_t io_base;
    int      ports;
    /* Physical addresses, full width so a test can check the below-4 GB
     * rule rather than see an already-truncated value. */
    uint64_t frame_list;    /* what FRBASEADD should hold */
    uint64_t sched_page;    /* skeleton QHs + interrupt pipes */
    uint64_t ctrl_page;     /* control-transfer TDs + buffer */
    int      pipes;         /* interrupt-IN pipes open */
    uint32_t pipe_errors;   /* failed interrupt TDs seen by poll */
} uhci_info_t;

int uhci_count(void);
int uhci_get_info(int idx, uhci_info_t *out);
/* 1 if interrupt pipe `pipe` of controller `idx` is open and not halted --
 * its TD is either queued on the device or has completed and waits for the
 * next usb_poll() to collect it -- 0 if it stalled, -1 for a bad index. */
int uhci_pipe_live(int idx, int pipe);
