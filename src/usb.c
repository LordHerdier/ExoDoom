/*
 * usb.c — USB core: enumeration and class-driver binding (SCRUM-198).
 *
 * See usb.h. Enumeration is the textbook sequence (USB 2.0 §9.1.2), done
 * once per connected root port:
 *
 *   reset port -> GET_DESCRIPTOR(device, 8) at address 0 for bMaxPacketSize0
 *   -> SET_ADDRESS -> GET_DESCRIPTOR(device, 18) -> GET_DESCRIPTOR(config, 9)
 *   -> GET_DESCRIPTOR(config, wTotalLength) -> offer it to each class driver.
 *
 * A device no class driver claims stays addressed but unconfigured. That is
 * where the mouse (SCRUM-246) and the stick (SCRUM-247) sit for now.
 */

#include "usb.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "serial.h"
#include "string.h"

#include <stdint.h>
#include <stddef.h>

#define USB_MAX_HCDS 8

static usb_hcd_t   *g_hcds[USB_MAX_HCDS];
static int          g_nhcds;
static usb_device_t g_devices[USB_MAX_DEVICES];
static int          g_ndevices;
static uint8_t      g_next_address;
static volatile int g_live;     /* usb_poll() does nothing until set */

/* Class drivers, offered every device in turn. Each returns 1 if it
 * claimed the device. SCRUM-246/-247 add theirs here. */
typedef struct {
    const char *name;
    int (*probe)(usb_device_t *dev, const uint8_t *config, uint16_t len);
} usb_class_driver_t;

static const usb_class_driver_t g_drivers[] = {
    { "usb_kbd", usb_kbd_probe },
};

int usb_hcd_register(usb_hcd_t *hcd)
{
    if (g_nhcds >= USB_MAX_HCDS) {
        return USB_ENOMEM;
    }
    hcd->index = g_nhcds;
    g_hcds[g_nhcds++] = hcd;
    return USB_OK;
}

int usb_control(const usb_device_t *dev, uint8_t request_type,
                uint8_t request, uint16_t value, uint16_t index,
                void *data, uint16_t length, uint16_t *actual)
{
    usb_setup_t setup = {
        .bmRequestType = request_type,
        .bRequest      = request,
        .wValue        = value,
        .wIndex        = index,
        .wLength       = length,
    };

    return dev->hcd->ops->control(dev->hcd, dev, &setup, data, actual);
}

static int get_descriptor(const usb_device_t *dev, uint8_t type,
                          void *buf, uint16_t len)
{
    uint16_t got = 0;
    int rc = usb_control(dev, USB_RT_IN | USB_RT_STANDARD | USB_RT_DEVICE,
                         USB_REQ_GET_DESCRIPTOR, (uint16_t)(type << 8), 0,
                         buf, len, &got);

    if (rc == USB_OK && got < len) {
        rc = USB_EIO;
    }
    return rc;
}

int usb_set_configuration(const usb_device_t *dev, uint8_t value)
{
    return usb_control(dev, USB_RT_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
                       USB_REQ_SET_CONFIGURATION, value, 0, NULL, 0, NULL);
}

int usb_config_find_interface(const uint8_t *config, uint16_t len,
                              uint8_t cls, uint8_t subclass, uint8_t protocol,
                              const usb_interface_desc_t **iface,
                              const usb_endpoint_desc_t **intr_in)
{
    const usb_interface_desc_t *match = NULL;

    if (iface) {
        *iface = NULL;
    }
    if (intr_in) {
        *intr_in = NULL;
    }

    for (uint16_t off = 0; off + 2 <= len;) {
        uint8_t blen = config[off];
        uint8_t type = config[off + 1];
        if (blen < 2 || off + blen > len) {
            break;                    /* malformed: stop, don't guess */
        }

        if (type == USB_DESC_INTERFACE
            && blen >= sizeof(usb_interface_desc_t)) {
            if (match) {
                break;                /* next interface: done with ours */
            }
            const usb_interface_desc_t *d =
                (const usb_interface_desc_t *)(config + off);
            if (d->bInterfaceClass == cls && d->bInterfaceSubClass == subclass
                && d->bInterfaceProtocol == protocol
                && d->bAlternateSetting == 0) {
                match = d;
                if (iface) {
                    *iface = d;
                }
            }
        } else if (match && type == USB_DESC_ENDPOINT
                   && blen >= sizeof(usb_endpoint_desc_t)) {
            const usb_endpoint_desc_t *e =
                (const usb_endpoint_desc_t *)(config + off);
            if ((e->bEndpointAddress & USB_EP_DIR_IN)
                && (e->bmAttributes & USB_EP_XFER_MASK) == USB_EP_XFER_INTR
                && intr_in && *intr_in == NULL) {
                *intr_in = e;
            }
        }
        off = (uint16_t)(off + blen);
    }
    return match ? USB_OK : USB_ENODEV;
}

static void log_port(const usb_hcd_t *hcd, int port)
{
    serial_print("usb: hc ");
    serial_print_dec((uint32_t)hcd->index);
    serial_print(" port ");
    serial_print_dec((uint32_t)port);
    serial_print(": ");
}

void usb_log_rc(const char *what, int rc)
{
    serial_print(what);
    serial_print(" failed (rc=");
    serial_print_hex64((uint64_t)(int64_t)rc);
    serial_print(")\n");
}

/* Give up on a device that never got an address. It is still enabled and
 * answering at address 0, where the next port's device is about to be
 * addressed too, so it has to come off the bus first. */
static void abandon_default(usb_hcd_t *hcd, int port, const char *what,
                            int rc)
{
    usb_log_rc(what, rc);
    hcd->ops->port_disable(hcd, port);
}

static void enumerate_port(usb_hcd_t *hcd, int port)
{
    uint8_t speed = USB_SPEED_FULL;

    /* Checked before the reset: an un-reset port stays disabled (HCRESET
     * left it so), so a device with no table slot never reaches address 0. */
    if (g_ndevices >= USB_MAX_DEVICES || g_next_address > 127) {
        log_port(hcd, port);
        serial_print("device table full, port left disabled\n");
        return;
    }

    int rc = hcd->ops->port_reset(hcd, port, &speed);
    if (rc == USB_ENODEV) {
        return;                       /* nothing plugged in */
    }
    log_port(hcd, port);
    if (rc != USB_OK) {
        abandon_default(hcd, port, "port reset", rc);
        return;
    }

    usb_device_t *dev = &g_devices[g_ndevices];
    memset(dev, 0, sizeof(*dev));
    dev->hcd = hcd;
    dev->port = (uint8_t)port;
    dev->speed = speed;
    dev->address = 0;
    dev->max_packet0 = 8;             /* every device accepts 8 at first */

    usb_device_desc_t dd;
    if ((rc = get_descriptor(dev, USB_DESC_DEVICE, &dd, 8)) != USB_OK) {
        abandon_default(hcd, port, "GET_DESCRIPTOR(device, 8)", rc);
        return;
    }
    if (dd.bMaxPacketSize0 == 8 || dd.bMaxPacketSize0 == 16
        || dd.bMaxPacketSize0 == 32 || dd.bMaxPacketSize0 == 64) {
        dev->max_packet0 = dd.bMaxPacketSize0;
    }

    uint8_t addr = g_next_address;
    rc = usb_control(dev, USB_RT_OUT | USB_RT_STANDARD | USB_RT_DEVICE,
                     USB_REQ_SET_ADDRESS, addr, 0, NULL, 0, NULL);
    if (rc != USB_OK) {
        abandon_default(hcd, port, "SET_ADDRESS", rc);
        return;
    }
    dev->address = addr;
    g_next_address++;
    hcd->ops->delay_ms(hcd, 2);       /* TSETADDR recovery (§9.2.6.3) */

    if ((rc = get_descriptor(dev, USB_DESC_DEVICE, &dd, sizeof(dd)))
        != USB_OK) {
        usb_log_rc("GET_DESCRIPTOR(device)", rc);
        return;
    }
    dev->vendor_id = dd.idVendor;
    dev->product_id = dd.idProduct;
    dev->dev_class = dd.bDeviceClass;

    static uint8_t config[USB_CONFIG_MAX];
    usb_config_desc_t *cd = (usb_config_desc_t *)config;
    if ((rc = get_descriptor(dev, USB_DESC_CONFIG, config, sizeof(*cd)))
        != USB_OK) {
        usb_log_rc("GET_DESCRIPTOR(config, 9)", rc);
        return;
    }
    uint16_t total = cd->wTotalLength;
    if (total < sizeof(*cd)) {
        total = sizeof(*cd);
    }
    if (total > USB_CONFIG_MAX) {
        total = USB_CONFIG_MAX;       /* interfaces past the cap are lost */
    }
    if ((rc = get_descriptor(dev, USB_DESC_CONFIG, config, total))
        != USB_OK) {
        usb_log_rc("GET_DESCRIPTOR(config)", rc);
        return;
    }
    dev->config_value = cd->bConfigurationValue;

    /* First interface's triple, for usb_dump() only. */
    for (uint16_t off = 0; off + 2 <= total && config[off] >= 2;
         off = (uint16_t)(off + config[off])) {
        if (config[off + 1] == USB_DESC_INTERFACE
            && off + sizeof(usb_interface_desc_t) <= total) {
            const usb_interface_desc_t *d =
                (const usb_interface_desc_t *)(config + off);
            dev->iface_class = d->bInterfaceClass;
            dev->iface_subclass = d->bInterfaceSubClass;
            dev->iface_protocol = d->bInterfaceProtocol;
            break;
        }
    }

    g_ndevices++;

    for (size_t i = 0; i < sizeof(g_drivers) / sizeof(g_drivers[0]); i++) {
        if (g_drivers[i].probe(dev, config, total)) {
            dev->driver = g_drivers[i].name;
            break;
        }
    }

    serial_print("addr ");
    serial_print_dec(dev->address);
    serial_print(" id ");
    serial_print_hex(dev->vendor_id);
    serial_print(":");
    serial_print_hex(dev->product_id);
    serial_print(" iface ");
    serial_print_hex(dev->iface_class);
    serial_print("/");
    serial_print_hex(dev->iface_subclass);
    serial_print("/");
    serial_print_hex(dev->iface_protocol);
    serial_print(dev->driver ? " -> " : " (unclaimed)");
    if (dev->driver) {
        serial_print(dev->driver);
    }
    serial_print("\n");
}

int usb_init(void)
{
    g_live = 0;
    g_nhcds = 0;
    g_ndevices = 0;
    g_next_address = 1;
    usb_kbd_reset();

    /* SCRUM-248 adds xhci_probe_all() next to this. */
    uhci_probe_all();
    if (g_nhcds == 0) {
        return USB_ENODEV;
    }

    for (int h = 0; h < g_nhcds; h++) {
        usb_hcd_t *hcd = g_hcds[h];
        int ports = hcd->ops->port_count(hcd);
        for (int p = 0; p < ports; p++) {
            enumerate_port(hcd, p);
        }
    }

    __asm__ volatile ("" ::: "memory");
    g_live = 1;
    return USB_OK;
}

void usb_poll(void)
{
    if (!g_live) {
        return;
    }
    for (int h = 0; h < g_nhcds; h++) {
        g_hcds[h]->ops->poll(g_hcds[h]);
    }
}

int usb_hcd_count(void)
{
    return g_nhcds;
}

usb_hcd_t *usb_hcd_at(int idx)
{
    return (idx >= 0 && idx < g_nhcds) ? g_hcds[idx] : NULL;
}

int usb_device_count(void)
{
    return g_ndevices;
}

const usb_device_t *usb_device_at(int idx)
{
    return (idx >= 0 && idx < g_ndevices) ? &g_devices[idx] : NULL;
}

void usb_dump(void)
{
    serial_print("usb: ");
    serial_print_dec((uint32_t)g_nhcds);
    serial_print(" controller(s), ");
    serial_print_dec((uint32_t)g_ndevices);
    serial_print(" device(s), ");
    serial_print_dec((uint32_t)usb_kbd_count());
    serial_print(" keyboard(s)\n");
}
