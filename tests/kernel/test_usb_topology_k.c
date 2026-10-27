/*
 * test_usb_topology_k.c — the QEMU USB topology SCRUM-245 settled on.
 *
 * Asserts what the spike decided and the PCI layer can see: the three UHCI
 * controllers QEMU_USB_TEST_ARGS in the Makefile attaches are there, sit on
 * an IRQ line the polled design can share, expose the I/O register block the
 * driver needs, never raise an interrupt, and -- now that SCRUM-198's driver
 * has taken them from SeaBIOS -- run the kernel's own schedule rather than
 * the firmware's. docker-test/docker-ci always pass those devices, so every
 * assertion is unconditional -- a failure here means the command line
 * drifted from docs/drivers/usb.md section 2, not that the machine has no
 * USB.
 */

#include "kunit.h"
#include "pci.h"
#include "uhci.h"
#include "serial.h"
#include "io.h"

#include <stdint.h>
#include <stddef.h>

/* QEMU_USB_TEST_ARGS: ich9-usb-uhci1..3. */
#define EXPECTED_UHCI 3

/* The I/O block is 32 bytes: USBCMD..PORTSC2 plus padding. */
#define UHCI_IO_SIZE 0x20

/* Every UHCI function PCI reports, in discovery order. One scan per test:
 * pci_init() rebuilds the table, so pointers from an earlier scan are not
 * kept across tests. */
static int find_uhci(const pci_device_t *out[EXPECTED_UHCI + 1])
{
    int n = 0;

    pci_init();
    for (int i = 0; i < pci_device_count() && n <= EXPECTED_UHCI; i++) {
        const pci_device_t *d = pci_device_at(i);
        if (d->class_code == PCI_CLASS_SERIAL_BUS
            && d->subclass == PCI_SUBCLASS_USB
            && d->prog_if == PCI_PROGIF_UHCI) {
            out[n++] = d;
        }
    }
    return n;
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
     * interrupt source. Checked on the register itself -- src/uhci.c writes
     * 0 there during takeover and never sets it -- rather than on PCI
     * Status' instantaneous INTx bit -- that one reads 0
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

static void test_controllers_run_kernel_schedule(void)
{
    const pci_device_t *uhci[EXPECTED_UHCI + 1];
    int n = find_uhci(uhci);

    /* SeaBIOS leaves every controller running its own schedule in reserved
     * RAM (docs/drivers/usb.md section 6). After usb_init() each must have
     * been reset and restarted on the frame list src/uhci.c allocated --
     * i.e. no firmware structure is still being DMA'd. */
    CU_ASSERT_EQUAL(uhci_count(), n);
    for (int i = 0; i < n; i++) {
        const pci_device_t *d = uhci[i];
        uhci_info_t info;
        int found = 0;

        for (int h = 0; h < uhci_count(); h++) {
            if (uhci_get_info(h, &info) == 0 && info.bus == d->bus
                && info.device == d->device
                && info.function == d->function) {
                found = 1;
                break;
            }
        }
        CU_ASSERT(found);
        if (!found) {
            continue;
        }

        uint32_t fl = inl(info.io_base + UHCI_FRBASEADD) & ~0xFFFu;
        print_dev(d);
        serial_print(" frbase 0x");
        serial_print_hex(fl);
        serial_print(" driver 0x");
        serial_print_hex((uint32_t)info.frame_list);
        serial_print("\n");

        CU_ASSERT_EQUAL((uint64_t)fl, info.frame_list);
        CU_ASSERT(inw(info.io_base + UHCI_USBCMD) & UHCI_CMD_RS);
        CU_ASSERT_EQUAL(inw(info.io_base + UHCI_USBSTS) & UHCI_STS_HCH, 0);
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
    CU_add_test(s, "UHCI controllers run the kernel's schedule",
                test_controllers_run_kernel_schedule);
}
