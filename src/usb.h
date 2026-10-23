#pragma once
#include <stdint.h>

/*
 * usb — USB core: host-controller interface, enumeration, device table
 * (SCRUM-198, epic SCRUM-243; design in docs/drivers/usb.md).
 *
 * Ring 0 only and syscall-free, like src/pci.c and src/hda.c. Three layers:
 *
 *   1. Host controller drivers (src/uhci.c today, xHCI in SCRUM-248) fill in
 *      a usb_hcd_t with the ops below and register it. The core never
 *      touches a controller register.
 *   2. This core walks every registered controller's root ports, gives each
 *      connected device an address, reads its descriptors and records it in
 *      a fixed table. There is no hub driver (docs/drivers/usb.md section 4),
 *      so a device is always one root port, one address.
 *   3. Class drivers (src/usb_kbd.c today; mouse SCRUM-246, mass storage
 *      SCRUM-247) are offered each device's configuration descriptor and
 *      claim what they recognise.
 *
 * Everything runs once, from usb_init() in kernel_main, with interrupts off:
 * control transfers are synchronous and busy-wait. After that, the only
 * thing that runs is usb_poll() from irq0_handler(), which services
 * interrupt-IN pipes within the budget docs/drivers/usb.md section 3 sets.
 */

#define USB_OK        0
#define USB_ENODEV  (-1)   /* nothing connected / no controller           */
#define USB_ETIMEOUT (-2)  /* transfer did not complete in time           */
#define USB_ESTALL  (-3)   /* device answered STALL                       */
#define USB_EIO     (-4)   /* CRC/timeout, babble, bitstuff, buffer error */
#define USB_ENOMEM  (-5)   /* out of pages, table slots or pipe slots     */
#define USB_EINVAL  (-6)   /* bad argument                                */
#define USB_ENOTSUP (-7)   /* op not implemented by this controller yet   */

#define USB_SPEED_FULL 0
#define USB_SPEED_LOW  1

/* ── Standard requests (USB 2.0 §9.4) ─────────────────────────────────── */
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_SET_CONFIGURATION 0x09

/* bmRequestType: direction | type | recipient. */
#define USB_RT_IN        0x80
#define USB_RT_OUT       0x00
#define USB_RT_STANDARD  0x00
#define USB_RT_CLASS     0x20
#define USB_RT_DEVICE    0x00
#define USB_RT_INTERFACE 0x01

#define USB_DESC_DEVICE    1
#define USB_DESC_CONFIG    2
#define USB_DESC_INTERFACE 4
#define USB_DESC_ENDPOINT  5

#define USB_EP_DIR_IN      0x80
#define USB_EP_NUM_MASK    0x0F
#define USB_EP_XFER_MASK   0x03
#define USB_EP_XFER_INTR   0x03

/* Largest configuration descriptor (with every interface/endpoint behind
 * it) the core will read. QEMU's devices are well under 100 bytes. */
#define USB_CONFIG_MAX 256

#define USB_MAX_DEVICES 16

typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

typedef struct usb_hcd usb_hcd_t;
typedef struct usb_device usb_device_t;

/* Called from usb_poll() -- that is, from irq0_handler() with IF clear --
 * with each completed interrupt-IN packet. Must stay inside the ISR budget
 * of docs/drivers/usb.md section 3: copy, queue, return. */
typedef void (*usb_intr_cb_t)(void *ctx, const uint8_t *data, uint32_t len);

/*
 * The host-controller interface. Every op takes the controller itself;
 * `port` is a 0-based root port index.
 */
typedef struct {
    const char *name;
    int  (*port_count)(usb_hcd_t *hcd);
    /* Reset and enable `port`. USB_OK with *speed set if a device is there
     * and the port came up enabled; USB_ENODEV if nothing is connected. */
    int  (*port_reset)(usb_hcd_t *hcd, int port, uint8_t *speed);
    /* Synchronous control transfer to `dev` endpoint 0. `data` is
     * setup->wLength bytes; *actual (may be NULL) gets the data-stage
     * length actually moved. */
    int  (*control)(usb_hcd_t *hcd, const usb_device_t *dev,
                    const usb_setup_t *setup, void *data, uint16_t *actual);
    /* Start polling interrupt-IN endpoint `ep` (number, no direction bit)
     * of `dev`; every completed packet goes to cb(ctx, ...). Returns the
     * controller's pipe index (>= 0) or a negative USB_E*. */
    int  (*interrupt_open)(usb_hcd_t *hcd, const usb_device_t *dev,
                           uint8_t ep, uint16_t max_packet,
                           usb_intr_cb_t cb, void *ctx);
    /* Synchronous bulk transfer. USB_ENOTSUP until SCRUM-247. */
    int  (*bulk)(usb_hcd_t *hcd, const usb_device_t *dev, uint8_t ep_addr,
                 void *data, uint32_t len, uint32_t *actual);
    /* IRQ0 service: check and re-arm interrupt pipes. */
    void (*poll)(usb_hcd_t *hcd);
    /* Busy-wait `ms` milliseconds on the controller's own frame clock --
     * enumeration runs with interrupts off, so the PIT is no use. */
    void (*delay_ms)(usb_hcd_t *hcd, uint32_t ms);
} usb_hcd_ops_t;

struct usb_hcd {
    const usb_hcd_ops_t *ops;
    void *priv;
    int   index;          /* registration order, set by usb_hcd_register() */
};

struct usb_device {
    usb_hcd_t *hcd;
    uint8_t  port;          /* 0-based root port on hcd             */
    uint8_t  address;       /* 1..127                               */
    uint8_t  speed;         /* USB_SPEED_*                          */
    uint8_t  max_packet0;   /* endpoint 0's bMaxPacketSize0         */
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  dev_class;
    uint8_t  config_value;  /* bConfigurationValue of config 0      */
    uint8_t  iface_class;   /* first interface, for usb_dump()      */
    uint8_t  iface_subclass;
    uint8_t  iface_protocol;
    const char *driver;     /* class driver that claimed it, or NULL */
};

/* Register a controller; called by HCD probe functions from usb_init().
 * Returns USB_OK or USB_ENOMEM when the table is full. */
int usb_hcd_register(usb_hcd_t *hcd);

/* Find every controller, enumerate every root port, bind class drivers,
 * then turn on usb_poll(). Returns USB_OK if at least one controller came
 * up (even with nothing attached), USB_ENODEV if none did. */
int usb_init(void);

/* IRQ0 hook. A no-op until usb_init() has finished. */
void usb_poll(void);

int usb_hcd_count(void);
usb_hcd_t *usb_hcd_at(int idx);
int usb_device_count(void);
const usb_device_t *usb_device_at(int idx);

/* Standard requests on endpoint 0, for class drivers. */
int usb_control(const usb_device_t *dev, uint8_t request_type,
                uint8_t request, uint16_t value, uint16_t index,
                void *data, uint16_t length, uint16_t *actual);
int usb_set_configuration(const usb_device_t *dev, uint8_t value);

/* Walk a configuration descriptor blob for the first interface matching
 * class/subclass/protocol, and that interface's first interrupt-IN
 * endpoint. Either out pointer may be NULL. USB_OK or USB_ENODEV. */
int usb_config_find_interface(const uint8_t *config, uint16_t len,
                              uint8_t cls, uint8_t subclass, uint8_t protocol,
                              const usb_interface_desc_t **iface,
                              const usb_endpoint_desc_t **intr_in);

/* Print the controller and device tables on serial. */
void usb_dump(void);
