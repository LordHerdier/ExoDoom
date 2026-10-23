/*
 * test_usb_topology_k.c — the QEMU USB topology SCRUM-245 settled on.
 *
 * There is no USB driver yet (SCRUM-198 brings one), so this suite asserts
 * only what the spike decided and the PCI layer can already see: the three
 * UHCI controllers QEMU_USB_TEST_ARGS in the Makefile attaches are there, sit
 * on an IRQ line the polled design can share, expose the I/O register block
 * the driver will need, and -- since SeaBIOS leaves them running -- neither
 * raise an interrupt nor DMA into RAM the kernel hands out. docker-test/docker-ci always pass those
 * devices, so every assertion is unconditional -- a failure here means the
 * command line drifted from docs/drivers/usb.md section 2, not that the
 * machine has no USB.
 */

#include "kunit.h"
#include "pci.h"
#include "serial.h"
#include "io.h"
#include "mmap.h"
#include "vmm.h"

#include <stdint.h>
#include <stddef.h>

/* Serial bus controller / USB / UHCI. Local until SCRUM-198 gives pci.h a
 * USB class define of its own. */
#define USB_CLASS     0x0C
#define USB_SUBCLASS  0x03
#define USB_PROG_UHCI 0x00

/* QEMU_USB_TEST_ARGS: ich9-usb-uhci1..3. */
#define EXPECTED_UHCI 3

/* UHCI's registers live in the I/O BAR at index 4 (USBBASE, config 0x20),
 * and the block is 32 bytes: USBCMD..PORTSC2 plus padding. */
#define UHCI_BAR_INDEX 4
#define UHCI_IO_SIZE   0x20

/* UHCI I/O registers, offsets from USBBASE. */
#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRBASEADD 0x08

/* UHCI link pointers (frame list entries, QH/TD links): bit 0 terminates,
 * bit 1 says the target is a QH rather than a TD, bits 31:4 the address. */
#define UHCI_LP_TERM   0x1u
#define UHCI_LP_QH     0x2u
#define UHCI_LP_ADDR   0xFFFFFFF0u
#define UHCI_FRAMES    1024

/* Schedule walk bounds: chains are short (a handful of QHs and TDs per
 * frame), and a controller schedule may loop back on itself. */
#define WALK_MAX_STEPS 64
#define WALK_MAX_DEPTH 4
#define WALK_MAX_MAPS  64

/* Every UHCI function PCI reports, in discovery order. One scan per test:
 * pci_init() rebuilds the table, so pointers from an earlier scan are not
 * kept across tests. */
static int find_uhci(const pci_device_t *out[EXPECTED_UHCI + 1])
{
    int n = 0;

    pci_init();
    for (int i = 0; i < pci_device_count() && n <= EXPECTED_UHCI; i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d->class_code == USB_CLASS && d->subclass == USB_SUBCLASS
            && d->prog_if == USB_PROG_UHCI) {
            out[n++] = d;
        }
    }
    return n;
}

/* ── Reading firmware's schedule ─────────────────────────────────────────
 * SeaBIOS leaves every UHCI controller running (USBCMD.RS) with its own
 * schedule, so until SCRUM-198 resets them the hardware keeps DMA'ing into
 * whatever that schedule points at. The kernel map covers only usable RAM,
 * so pages outside it are identity-mapped here for the walk and unmapped
 * after. */
static uint64_t walk_maps[WALK_MAX_MAPS];
static int      walk_nmaps;
static int      walk_overflow;
static uint32_t walk_checked;
static uint32_t walk_in_usable;
static uint32_t walk_first_bad;

static int peek32(uint32_t paddr, uint32_t *out)
{
    uint64_t page = paddr & ~(VMM_PAGE_SIZE - 1);

    if (vmm_translate(page, NULL, NULL) != VMM_OK) {
        if (walk_nmaps == WALK_MAX_MAPS
            || vmm_map_page(page, page, VMM_PRESENT) != VMM_OK) {
            walk_overflow = 1;
            return 0;
        }
        walk_maps[walk_nmaps++] = page;
    }
    *out = *(volatile uint32_t *)(uintptr_t)paddr;
    return 1;
}

static void walk_unmap_all(void)
{
    for (int i = 0; i < walk_nmaps; i++) {
        vmm_unmap_page(walk_maps[i]);
    }
    walk_nmaps = 0;
}

/* Count [addr, addr+len) as a DMA target; flag it if any byte lies in RAM
 * the memory map calls usable -- the only RAM page_alloc can hand out. */
static void check_target(uint32_t addr, uint32_t len)
{
    uint32_t count = 0;
    const mmap_region_t *r = mmap_get_regions(&count);

    walk_checked++;
    for (uint32_t i = 0; i < count; i++) {
        if (r[i].type != MULTIBOOT_MMAP_AVAILABLE) {
            continue;
        }
        if ((uint64_t)addr < r[i].base + r[i].length
            && (uint64_t)addr + len > r[i].base) {
            if (walk_in_usable++ == 0) {
                walk_first_bad = addr;
            }
            return;
        }
    }
}

/* Follow one link pointer: a horizontal chain of QHs/TDs, descending into
 * each QH's element chain. A TD's buffer is checked too -- that is where a
 * HID report actually lands. */
static void walk_link(uint32_t lp, int depth)
{
    for (int step = 0; step < WALK_MAX_STEPS && !(lp & UHCI_LP_TERM);
         step++) {
        uint32_t a = lp & UHCI_LP_ADDR;
        uint32_t next, w;

        if (lp & UHCI_LP_QH) {
            check_target(a, 8);
            if (depth < WALK_MAX_DEPTH && peek32(a + 4, &w)) {
                walk_link(w, depth + 1);            /* element chain */
            }
        } else {
            uint32_t token, buf;
            check_target(a, 16);
            if (peek32(a + 8, &token) && peek32(a + 12, &buf) && buf != 0) {
                /* MaxLen is bits 31:21, encoded n-1; 0x7FF means none. */
                uint32_t maxlen = ((token >> 21) + 1) & 0x7FF;
                if (maxlen != 0) {
                    check_target(buf, maxlen);
                }
            }
        }
        if (!peek32(a, &next)) {
            return;
        }
        lp = next;
    }
}

static void print_dev(const pci_device_t *d)
{
    serial_print("usb_topology: uhci ");
    serial_print_dec(d->device);
    serial_print(".");
    serial_print_dec(d->function);
}

static void test_three_uhci_controllers(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];

    /* 3 controllers x 2 root ports is the 6 ports docs/drivers/usb.md
     * section 4 counts on instead of a hub driver. */
    CU_ASSERT_EQUAL(find_uhci(uhci), EXPECTED_UHCI);
}

static void test_uhci_irq_lines_are_shareable(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];
    int n = find_uhci(uhci);

    for (int i = 0; i < n; i++) {
        const pci_device_t *d = uhci[i];
        uint8_t pin  = pci_config_read8(d->bus, d->device, d->function,
                                        PCI_OFF_INTERRUPT_PIN);
        uint8_t line = pci_config_read8(d->bus, d->device, d->function,
                                        PCI_OFF_INTERRUPT_LINE);

        print_dev(d);
        serial_print(" pin ");
        serial_print_dec(pin);
        serial_print(" irq ");
        serial_print_dec(line);
        serial_print("\n");

        /* Routed by firmware at all (INTA#..INTD#, a real 8259 line)... */
        CU_ASSERT(pin >= 1 && pin <= 4);
        CU_ASSERT(line <= 15);
        /* ...and not onto the timer, keyboard or cascade. The polled design
         * never installs a stub, so any other line -- HDA's included -- is
         * fine; these three are the ones hda.c refuses too. */
        CU_ASSERT(line != 0 && line != 1 && line != 2);
    }
}

static void test_uhci_io_bar_decodes(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];
    int n = find_uhci(uhci);

    for (int i = 0; i < n; i++) {
        pci_bar_t bar;
        CU_ASSERT_EQUAL(pci_bar_read(uhci[i], UHCI_BAR_INDEX, &bar), PCI_OK);
        CU_ASSERT_EQUAL(bar.is_io, 1);
        CU_ASSERT_EQUAL(bar.size, UHCI_IO_SIZE);
        CU_ASSERT_NOT_EQUAL(bar.base, 0);
    }
}

static void test_uhci_interrupts_disabled(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];
    int n = find_uhci(uhci);

    /* The claim docs/drivers/hda.md section 7 rests on: a UHCI function that
     * shares HDA's line can never raise it, because USBINTR enables no
     * interrupt source. Checked on the register itself, as firmware left it,
     * rather than on PCI Status' instantaneous INTx bit -- that one reads 0
     * between completions even with every source enabled. */
    for (int i = 0; i < n; i++) {
        const pci_device_t *d = uhci[i];
        pci_bar_t bar;
        if (pci_bar_read(d, UHCI_BAR_INDEX, &bar) != PCI_OK || !bar.is_io) {
            CU_ASSERT(0);   /* UHCI I/O BAR 4 unreadable */
            continue;
        }
        uint16_t cmd  = pci_config_read16(d->bus, d->device, d->function,
                                          PCI_OFF_COMMAND);
        uint16_t io   = (uint16_t)bar.base;
        uint16_t intr = inw(io + UHCI_USBINTR);

        print_dev(d);
        serial_print(" pcicmd ");
        serial_print_hex(cmd);
        serial_print(" usbcmd ");
        serial_print_hex(inw(io + UHCI_USBCMD));
        serial_print(" usbsts ");
        serial_print_hex(inw(io + UHCI_USBSTS));
        serial_print(" usbintr ");
        serial_print_hex(intr);
        serial_print(" frbase ");
        serial_print_hex(inl(io + UHCI_FRBASEADD));
        serial_print("\n");

        /* I/O decode must be on, or every read below is the bus's 0xFFFF. */
        CU_ASSERT(cmd & PCI_CMD_IO_SPACE);
        CU_ASSERT_EQUAL(intr & 0x000F, 0);
    }
}

static void test_firmware_schedule_avoids_usable_ram(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];
    int n = find_uhci(uhci);

    /* Running controllers the kernel does not own yet are only safe while
     * every frame list, QH, TD and data buffer they touch stays out of the
     * RAM page_alloc manages (docs/drivers/usb.md section 6). */
    for (int i = 0; i < n; i++) {
        pci_bar_t bar;
        if (pci_bar_read(uhci[i], UHCI_BAR_INDEX, &bar) != PCI_OK) {
            CU_ASSERT(0);   /* UHCI I/O BAR 4 unreadable */
            continue;
        }
        uint32_t fl = inl((uint16_t)bar.base + UHCI_FRBASEADD) & ~0xFFFu;

        walk_checked = walk_in_usable = walk_first_bad = 0;
        walk_overflow = 0;
        check_target(fl, UHCI_FRAMES * 4);
        for (uint32_t f = 0; f < UHCI_FRAMES; f++) {
            uint32_t lp;
            if (!peek32(fl + f * 4, &lp)) {
                break;
            }
            walk_link(lp, 0);
        }
        walk_unmap_all();

        print_dev(uhci[i]);
        serial_print(" schedule: ");
        serial_print_dec(walk_checked);
        serial_print(" targets, ");
        serial_print_dec(walk_in_usable);
        serial_print(" in usable RAM");
        if (walk_in_usable) {
            serial_print(" (first 0x");
            serial_print_hex(walk_first_bad);
            serial_print(")");
        }
        serial_print("\n");

        CU_ASSERT_EQUAL(walk_overflow, 0);
        CU_ASSERT_EQUAL(walk_in_usable, 0);
    }
}

void suite_usb_topology_tests(CU_pSuite s)
{
    CU_add_test(s, "three UHCI controllers are attached",
                test_three_uhci_controllers);
    CU_add_test(s, "UHCI IRQ lines avoid IRQ 0/1/2",
                test_uhci_irq_lines_are_shareable);
    CU_add_test(s, "UHCI I/O BAR 4 decodes as a 32-byte block",
                test_uhci_io_bar_decodes);
    CU_add_test(s, "UHCI interrupt sources are all disabled (USBINTR)",
                test_uhci_interrupts_disabled);
    CU_add_test(s, "firmware's live UHCI schedule stays out of usable RAM",
                test_firmware_schedule_avoids_usable_ram);
}
