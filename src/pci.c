#include "pci.h"
#include "io.h"
#include "serial.h"
#include "vmm.h"

#include <stddef.h>

/*
 * The CONFIG_ADDRESS layout (PCI 3.0, configuration mechanism #1):
 *
 *   bit 31    enable
 *   bits 30-24 reserved (must be zero)
 *   bits 23-16 bus
 *   bits 15-11 device
 *   bits 10-8  function
 *   bits 7-0   register offset, bits 1:0 always zero
 *
 * The two low offset bits are masked off here rather than asserted on:
 * every access below is aligned by construction, and masking keeps a
 * mis-aligned offset from silently addressing a *different* register.
 */
#define PCI_ADDR_ENABLE 0x80000000u

static uint32_t config_address(uint8_t bus, uint8_t dev, uint8_t fn,
                               uint8_t off)
{
    return PCI_ADDR_ENABLE
         | ((uint32_t)bus << 16)
         | (((uint32_t)dev & 0x1F) << 11)
         | (((uint32_t)fn  & 0x07) << 8)
         | ((uint32_t)off & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, off));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t dword = pci_config_read32(bus, dev, fn, off);
    return (uint16_t)((dword >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    uint32_t dword = pci_config_read32(bus, dev, fn, off);
    return (uint8_t)((dword >> ((off & 3) * 8)) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, off));
    outl(PCI_CONFIG_DATA, val);
}

/*
 * A 16-bit write is a read-modify-write of the containing dword: mechanism
 * #1's data port is 32 bits wide and the byte enables are not reachable from
 * software here, so writing the half alone is not an option.
 */
void pci_config_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off,
                        uint16_t val)
{
    uint32_t dword = pci_config_read32(bus, dev, fn, off);
    unsigned shift = (off & 2) * 8;
    dword &= ~(0xFFFFu << shift);
    dword |= (uint32_t)val << shift;
    pci_config_write32(bus, dev, fn, off, dword);
}

/* ── The device table ─────────────────────────────────────────────────── */

static pci_device_t g_devices[PCI_MAX_DEVICES];
static int g_device_count;
static int g_devices_skipped;

/* One bit per bus number, so the recursive scan below cannot revisit a bus
 * -- and so a bridge reporting a secondary bus it is itself behind cannot
 * recurse forever. 256 buses / 64 bits per word = 4 words. */
static uint64_t g_visited_bus[4];

static int bus_visited(uint8_t bus)
{
    return (g_visited_bus[bus >> 6] >> (bus & 63)) & 1;
}

static void mark_bus_visited(uint8_t bus)
{
    g_visited_bus[bus >> 6] |= 1ULL << (bus & 63);
}

static void scan_bus(uint8_t bus);

static void record_function(uint8_t bus, uint8_t dev, uint8_t fn,
                            uint16_t vendor, uint8_t raw_header_type)
{
    if (g_device_count >= PCI_MAX_DEVICES) {
        g_devices_skipped++;
        return;
    }

    pci_device_t *d = &g_devices[g_device_count++];
    d->bus        = bus;
    d->device     = dev;
    d->function   = fn;
    d->vendor_id  = vendor;
    d->device_id  = pci_config_read16(bus, dev, fn, PCI_OFF_DEVICE_ID);
    d->revision   = pci_config_read8(bus, dev, fn, PCI_OFF_REVISION);
    d->prog_if    = pci_config_read8(bus, dev, fn, PCI_OFF_PROG_IF);
    d->subclass   = pci_config_read8(bus, dev, fn, PCI_OFF_SUBCLASS);
    d->class_code = pci_config_read8(bus, dev, fn, PCI_OFF_CLASS);
    d->header_type   = raw_header_type & PCI_HEADER_TYPE_MASK;
    d->multifunction = (raw_header_type & PCI_HEADER_MULTIFUNC) ? 1 : 0;
}

static void check_function(uint8_t bus, uint8_t dev, uint8_t fn,
                           uint16_t vendor, uint8_t raw_header_type)
{
    record_function(bus, dev, fn, vendor, raw_header_type);

    /* A PCI-to-PCI bridge is the only thing that adds a bus to the scan.
     * Its secondary bus number is read, not assigned -- see pci.h. */
    uint8_t class_code = pci_config_read8(bus, dev, fn, PCI_OFF_CLASS);
    uint8_t subclass   = pci_config_read8(bus, dev, fn, PCI_OFF_SUBCLASS);
    if (class_code == PCI_CLASS_BRIDGE && subclass == PCI_SUBCLASS_PCI_BRIDGE) {
        uint8_t secondary = pci_config_read8(bus, dev, fn,
                                             PCI_OFF_SECONDARY_BUS);
        scan_bus(secondary);
    }
}

static void check_device(uint8_t bus, uint8_t dev)
{
    uint16_t vendor = pci_config_read16(bus, dev, 0, PCI_OFF_VENDOR_ID);
    if (vendor == PCI_VENDOR_NONE) {
        return;                       /* nothing in this slot */
    }

    uint8_t raw_header_type = pci_config_read8(bus, dev, 0,
                                               PCI_OFF_HEADER_TYPE);
    check_function(bus, dev, 0, vendor, raw_header_type);

    /* Function 0's MF bit is the only reliable way to know whether functions
     * 1-7 mean anything: a single-function device answers every function
     * number with function 0's own registers, so scanning them unconditionally
     * would record the same device eight times. */
    if (!(raw_header_type & PCI_HEADER_MULTIFUNC)) {
        return;
    }

    for (uint8_t fn = 1; fn < 8; fn++) {
        uint16_t fn_vendor = pci_config_read16(bus, dev, fn, PCI_OFF_VENDOR_ID);
        if (fn_vendor == PCI_VENDOR_NONE) {
            continue;                 /* functions need not be contiguous */
        }
        check_function(bus, dev, fn, fn_vendor,
                       pci_config_read8(bus, dev, fn, PCI_OFF_HEADER_TYPE));
    }
}

static void scan_bus(uint8_t bus)
{
    if (bus_visited(bus)) {
        return;
    }
    mark_bus_visited(bus);

    for (uint8_t dev = 0; dev < 32; dev++) {
        check_device(bus, dev);
    }
}

void pci_init(void)
{
    g_device_count    = 0;
    g_devices_skipped = 0;
    for (int i = 0; i < 4; i++) {
        g_visited_bus[i] = 0;
    }

    /* Bus 0 always exists if PCI does at all. A multi-function host bridge
     * at 0:0.0 means one host controller per function, each responsible for
     * the bus with its own function number -- so those buses are scanned
     * directly rather than waiting to be reached through a bridge. */
    uint8_t host_header = pci_config_read8(0, 0, 0, PCI_OFF_HEADER_TYPE);
    if (!(host_header & PCI_HEADER_MULTIFUNC)) {
        scan_bus(0);
    } else {
        for (uint8_t fn = 0; fn < 8; fn++) {
            if (pci_config_read16(0, 0, fn, PCI_OFF_VENDOR_ID)
                    == PCI_VENDOR_NONE) {
                break;
            }
            scan_bus(fn);
        }
    }

    if (g_devices_skipped > 0) {
        serial_print("pci: device table full, skipped ");
        serial_print_dec((uint32_t)g_devices_skipped);
        serial_print(" function(s)\n");
    }
}

int pci_device_count(void)   { return g_device_count; }
int pci_devices_skipped(void){ return g_devices_skipped; }

const pci_device_t *pci_device_at(int idx)
{
    if (idx < 0 || idx >= g_device_count) {
        return NULL;
    }
    return &g_devices[idx];
}

const pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass)
{
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].class_code == class_code
                && g_devices[i].subclass == subclass) {
            return &g_devices[i];
        }
    }
    return NULL;
}

const pci_device_t *pci_find_id(uint16_t vendor_id, uint16_t device_id)
{
    for (int i = 0; i < g_device_count; i++) {
        if (g_devices[i].vendor_id == vendor_id
                && g_devices[i].device_id == device_id) {
            return &g_devices[i];
        }
    }
    return NULL;
}

/* ── BARs ─────────────────────────────────────────────────────────────── */

#define PCI_BAR_IS_IO          0x1u
#define PCI_BAR_MEM_TYPE_MASK  0x6u
#define PCI_BAR_MEM_TYPE_64    0x4u
#define PCI_BAR_MEM_PREFETCH   0x8u
#define PCI_BAR_MEM_ADDR_MASK  0xFFFFFFF0u
#define PCI_BAR_IO_ADDR_MASK   0xFFFFFFFCu

static uint8_t bar_offset(int index)
{
    return (uint8_t)(PCI_OFF_BAR0 + index * 4);
}

int pci_bar_read(const pci_device_t *dev, int index, pci_bar_t *out)
{
    if (dev == NULL || out == NULL || index < 0 || index >= PCI_MAX_BARS) {
        return PCI_EINVAL;
    }
    /* Only the general header has six BARs at 0x10; a bridge has two and a
     * CardBus bridge none, and this driver does not try to size those. */
    if (dev->header_type != PCI_HEADER_GENERAL) {
        return PCI_EINVAL;
    }

    out->base = 0;
    out->size = 0;
    out->is_io = 0;
    out->is_64 = 0;
    out->prefetchable = 0;

    uint8_t off = bar_offset(index);
    uint32_t lo = pci_config_read32(dev->bus, dev->device, dev->function, off);
    if (lo == 0) {
        return PCI_ENODEV;            /* BAR not implemented */
    }

    int is_io = (lo & PCI_BAR_IS_IO) != 0;
    int is_64 = !is_io
             && (lo & PCI_BAR_MEM_TYPE_MASK) == PCI_BAR_MEM_TYPE_64;
    if (is_64 && index + 1 >= PCI_MAX_BARS) {
        /* A 64-bit BAR in the last slot has nowhere to keep its upper half:
         * malformed, not something to guess at. */
        return PCI_EINVAL;
    }

    uint32_t hi = is_64
        ? pci_config_read32(dev->bus, dev->device, dev->function, off + 4)
        : 0;

    /*
     * Sizing. The PCI spec warns that some devices decode the all-ones write
     * as an unintended access, so I/O and memory decode go off for the whole
     * sequence -- including the restore of the original BAR value -- and the
     * command register is put back exactly as it was afterwards.
     */
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function,
                                     PCI_OFF_COMMAND);
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_OFF_COMMAND,
                       (uint16_t)(cmd & ~(PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE)));

    pci_config_write32(dev->bus, dev->device, dev->function, off, 0xFFFFFFFFu);
    uint32_t size_lo = pci_config_read32(dev->bus, dev->device, dev->function,
                                         off);
    uint32_t size_hi = 0;
    if (is_64) {
        pci_config_write32(dev->bus, dev->device, dev->function, off + 4,
                           0xFFFFFFFFu);
        size_hi = pci_config_read32(dev->bus, dev->device, dev->function,
                                    off + 4);
        pci_config_write32(dev->bus, dev->device, dev->function, off + 4, hi);
    }
    pci_config_write32(dev->bus, dev->device, dev->function, off, lo);

    pci_config_write16(dev->bus, dev->device, dev->function, PCI_OFF_COMMAND,
                       cmd);

    uint64_t mask;
    if (is_io) {
        out->base = lo & PCI_BAR_IO_ADDR_MASK;
        mask = (uint64_t)(size_lo & PCI_BAR_IO_ADDR_MASK);
        /* An I/O BAR decodes 16 bits of port address at most, so the upper
         * bits of the readback carry no size information. */
        mask |= 0xFFFFFFFFFFFF0000ULL;
    } else {
        out->base = ((uint64_t)hi << 32) | (lo & PCI_BAR_MEM_ADDR_MASK);
        mask = ((uint64_t)size_hi << 32)
             | (uint64_t)(size_lo & PCI_BAR_MEM_ADDR_MASK);
        if (!is_64) {
            mask |= 0xFFFFFFFF00000000ULL;
        }
        out->prefetchable = (lo & PCI_BAR_MEM_PREFETCH) ? 1 : 0;
    }

    out->is_io = (uint8_t)is_io;
    out->is_64 = (uint8_t)is_64;

    /* Size is ~(writable address bits) + 1. A readback with no writable bits
     * at all means the BAR is not really there, whatever the original value
     * looked like. */
    if ((~mask) == 0) {
        out->size = 0;
        return PCI_ENODEV;
    }
    out->size = (~mask) + 1;
    return PCI_OK;
}

/*
 * The MMIO window pci_bar_assign() hands out of. It sits in the 32-bit PCI
 * hole QEMU leaves between the top of low RAM and the LAPIC/IOAPIC block at
 * 0xFEC00000 -- above any RAM this kernel is booted with (docker-run passes
 * -m 256M, and the hole starts at 0xC0000000 even on a 3 GB machine), and
 * clear of the framebuffer aperture GRUB requests, which QEMU places at
 * 0xFD000000. It is deliberately not derived from anything at runtime: this
 * is a QEMU-targeted kernel with no ACPI/_CRS parsing to ask, and a fixed
 * window that is checked into the source is auditable where a guessed one
 * would not be.
 */
#define PCI_MMIO_WINDOW_BASE 0xE0000000ULL
#define PCI_MMIO_WINDOW_END  0xF0000000ULL

static uint64_t g_mmio_next = PCI_MMIO_WINDOW_BASE;

int pci_bar_assign(const pci_device_t *dev, int index, pci_bar_t *out)
{
    if (dev == NULL || out == NULL) {
        return PCI_EINVAL;
    }

    int rc = pci_bar_read(dev, index, out);
    if (rc != PCI_OK) {
        return rc;
    }
    if (out->is_io) {
        return PCI_EINVAL;            /* port space is not ours to hand out */
    }

    /* A BAR is naturally aligned: its size is a power of two and its base
     * must be a multiple of it. */
    uint64_t base = (g_mmio_next + out->size - 1) & ~(out->size - 1);
    if (base + out->size > PCI_MMIO_WINDOW_END) {
        return PCI_ENOSPC;
    }

    uint8_t off = bar_offset(index);
    uint32_t lo = pci_config_read32(dev->bus, dev->device, dev->function, off);
    /* Keep the BAR's own type/prefetch bits; only the address changes. */
    pci_config_write32(dev->bus, dev->device, dev->function, off,
                       (uint32_t)(base & PCI_BAR_MEM_ADDR_MASK)
                           | (lo & ~PCI_BAR_MEM_ADDR_MASK));
    if (out->is_64) {
        pci_config_write32(dev->bus, dev->device, dev->function, off + 4,
                           (uint32_t)(base >> 32));
    }

    g_mmio_next = base + out->size;
    out->base = base;
    return PCI_OK;
}

int pci_enable_device(const pci_device_t *dev)
{
    if (dev == NULL) {
        return PCI_EINVAL;
    }

    uint16_t bits = PCI_CMD_BUS_MASTER;

    if (dev->header_type == PCI_HEADER_GENERAL) {
        for (int i = 0; i < PCI_MAX_BARS; i++) {
            pci_bar_t bar;
            if (pci_bar_read(dev, i, &bar) != PCI_OK) {
                continue;
            }
            bits |= bar.is_io ? PCI_CMD_IO_SPACE : PCI_CMD_MEM_SPACE;
            if (bar.is_64) {
                i++;                  /* the upper half is not its own BAR */
            }
        }
    } else {
        /* A bridge has BARs this driver does not size; enabling both
         * decoders is what a bridge needs to forward anything at all. */
        bits |= PCI_CMD_IO_SPACE | PCI_CMD_MEM_SPACE;
    }

    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function,
                                     PCI_OFF_COMMAND);
    pci_config_write16(dev->bus, dev->device, dev->function, PCI_OFF_COMMAND,
                       (uint16_t)(cmd | bits));
    return PCI_OK;
}

int pci_bar_map(const pci_bar_t *bar)
{
    if (bar == NULL || bar->is_io || bar->base == 0 || bar->size == 0) {
        return PCI_EINVAL;
    }

    /* Round out to whole pages: a BAR may be smaller than 4 KiB (HDA's is
     * 16 KiB, but a small one is legal) and vmm_map_range wants page
     * granularity either way. */
    uint64_t start = bar->base & ~(VMM_PAGE_SIZE - 1);
    uint64_t end   = (bar->base + bar->size + VMM_PAGE_SIZE - 1)
                     & ~(VMM_PAGE_SIZE - 1);

    /* PCD: MMIO registers have read side effects and must not be cached.
     * Prefetchable BARs could be write-through, which is a later
     * optimisation and not worth a second code path here. */
    int rc = vmm_map_range(start, start, end - start,
                           VMM_PRESENT | VMM_WRITE | VMM_PCD);
    if (rc == VMM_OK) {
        return PCI_OK;
    }
    return (rc == VMM_ENOMEM) ? PCI_ENOSPC : PCI_EINVAL;
}

/* serial_print_hex() always emits 8 digits, which turns a class byte into
 * "00000006". One line per function is easier to scan at boot with each
 * field printed at its natural width. */
static void print_hexn(uint32_t val, int digits)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        serial_putc(hex[(val >> (i * 4)) & 0xF]);
    }
}

void pci_dump(void)
{
    serial_print("pci: ");
    serial_print_dec((uint32_t)g_device_count);
    serial_print(" function(s)\n");

    for (int i = 0; i < g_device_count; i++) {
        const pci_device_t *d = &g_devices[i];
        serial_print("pci:   ");
        print_hexn(d->bus, 2);
        serial_print(":");
        print_hexn(d->device, 2);
        serial_print(".");
        print_hexn(d->function, 1);
        serial_print("  ");
        print_hexn(d->vendor_id, 4);
        serial_print(":");
        print_hexn(d->device_id, 4);
        serial_print("  class ");
        print_hexn(d->class_code, 2);
        print_hexn(d->subclass, 2);
        serial_print(" prog_if ");
        print_hexn(d->prog_if, 2);
        serial_print(" hdr ");
        print_hexn(d->header_type, 2);
        serial_print("\n");
    }
}
