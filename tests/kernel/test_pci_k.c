/*
 * test_pci_k.c — PCI enumeration and config-space driver (SCRUM-209).
 *
 * Drives the real pci_init()/pci_config_*()/pci_bar_read() against whatever
 * QEMU actually emulates, not a mock: there is no way to fake a host bridge
 * on ports 0xCF8/0xCFC from inside the kernel, and a mock would have proved
 * nothing about the one thing this ticket is for.
 *
 * The acceptance case is the Intel HDA controller: docker-test/docker-ci
 * pass `-device intel-hda` (see Makefile), so the audio-controller
 * assertions below are unconditional -- a build that stops finding it is a
 * regression in enumeration, not a machine without a sound card. Every
 * other assertion holds on any QEMU machine (an i440FX host bridge, an ISA
 * bridge and an IDE controller are always there).
 */

#include "kunit.h"
#include "pci.h"
#include "vmm.h"

#include <stdint.h>
#include <stddef.h>

/* Bus 255, device 31, function 7 is the last addressable function on the
 * last bus -- nothing QEMU emulates lives there, so it stands in for "a
 * function that does not exist". */
#define ABSENT_BUS 255
#define ABSENT_DEV 31
#define ABSENT_FN  7

static void test_host_bridge_is_present(void)
{
    /* 0:0.0 is the host bridge on every PCI machine; QEMU's default
     * i440FX reports 8086:1237. Only the "answered at all" part is asserted
     * -- the machine type is not this driver's business. */
    uint16_t vendor = pci_config_read16(0, 0, 0, PCI_OFF_VENDOR_ID);
    CU_ASSERT_NOT_EQUAL(vendor, PCI_VENDOR_NONE);
    CU_ASSERT_NOT_EQUAL(vendor, 0x0000);
}

static void test_absent_function_reads_all_ones(void)
{
    /* The host bridge completes a config access to a device that is not
     * there without error, returning all ones. That is the entire basis of
     * the enumeration probe, so it is worth asserting directly. */
    CU_ASSERT_EQUAL(pci_config_read32(ABSENT_BUS, ABSENT_DEV, ABSENT_FN,
                                      PCI_OFF_VENDOR_ID), 0xFFFFFFFFu);
    CU_ASSERT_EQUAL(pci_config_read16(ABSENT_BUS, ABSENT_DEV, ABSENT_FN,
                                      PCI_OFF_VENDOR_ID), PCI_VENDOR_NONE);
}

static void test_read16_halves_match_read32(void)
{
    /* Offset 0x00 holds vendor (low half) and device (high half) in one
     * dword: the shift/mask the 8- and 16-bit accessors do on top of
     * mechanism #1's mandatory 32-bit access is the thing being checked. */
    uint32_t dword = pci_config_read32(0, 0, 0, PCI_OFF_VENDOR_ID);
    CU_ASSERT_EQUAL(pci_config_read16(0, 0, 0, PCI_OFF_VENDOR_ID),
                    (uint16_t)(dword & 0xFFFF));
    CU_ASSERT_EQUAL(pci_config_read16(0, 0, 0, PCI_OFF_DEVICE_ID),
                    (uint16_t)(dword >> 16));
    CU_ASSERT_EQUAL(pci_config_read8(0, 0, 0, PCI_OFF_VENDOR_ID),
                    (uint8_t)(dword & 0xFF));
    CU_ASSERT_EQUAL(pci_config_read8(0, 0, 0, PCI_OFF_VENDOR_ID + 1),
                    (uint8_t)((dword >> 8) & 0xFF));
}

static void test_enumeration_finds_devices(void)
{
    pci_init();

    /* QEMU's bare i440FX machine already has a host bridge, an ISA bridge
     * and an IDE controller before any -device flag. */
    CU_ASSERT(pci_device_count() >= 3);
    CU_ASSERT_EQUAL(pci_devices_skipped(), 0);
    CU_ASSERT_PTR_NOT_NULL(pci_device_at(0));
    CU_ASSERT_PTR_NULL(pci_device_at(pci_device_count()));
    CU_ASSERT_PTR_NULL(pci_device_at(-1));
}

static void test_no_duplicate_functions_recorded(void)
{
    pci_init();

    /* The MF-bit check in check_device() is what stops a single-function
     * device being recorded eight times, and the visited-bus bitmap is what
     * stops a bus being walked twice. Both failures look identical from
     * here: the same bus/device/function appearing more than once. */
    for (int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *a = pci_device_at(i);
        for (int j = i + 1; j < pci_device_count(); j++) {
            const pci_device_t *b = pci_device_at(j);
            CU_ASSERT(!(a->bus == b->bus && a->device == b->device
                        && a->function == b->function));
        }
    }
}

static void test_host_bridge_recorded_with_right_class(void)
{
    pci_init();

    const pci_device_t *d = pci_device_at(0);
    CU_ASSERT_PTR_NOT_NULL(d);
    if (d == NULL) {
        return;
    }
    /* Discovery order starts at bus 0, device 0, function 0. */
    CU_ASSERT_EQUAL(d->bus, 0);
    CU_ASSERT_EQUAL(d->device, 0);
    CU_ASSERT_EQUAL(d->function, 0);
    CU_ASSERT_EQUAL(d->class_code, PCI_CLASS_BRIDGE);
    CU_ASSERT_EQUAL(d->subclass, 0x00);          /* host bridge */
    CU_ASSERT_EQUAL(d->header_type, PCI_HEADER_GENERAL);
}

/* ── The acceptance case: QEMU's -device intel-hda ────────────────────── */

static const pci_device_t *find_audio(void)
{
    pci_init();
    return pci_find_class(PCI_CLASS_MULTIMEDIA, PCI_SUBCLASS_AUDIO_DEV);
}

static void test_finds_audio_controller(void)
{
    const pci_device_t *hda = find_audio();

    CU_ASSERT_PTR_NOT_NULL(hda);
    if (hda == NULL) {
        return;
    }
    /* Class 0x0403 is what the ticket names; the vendor is Intel, and
     * pci_find_id() must agree with pci_find_class() about the same
     * function. */
    CU_ASSERT_EQUAL(hda->class_code, PCI_CLASS_MULTIMEDIA);
    CU_ASSERT_EQUAL(hda->subclass, PCI_SUBCLASS_AUDIO_DEV);
    CU_ASSERT_EQUAL(hda->vendor_id, 0x8086);
    CU_ASSERT_EQUAL(hda->header_type, PCI_HEADER_GENERAL);
    CU_ASSERT_EQUAL(pci_find_id(hda->vendor_id, hda->device_id), hda);
}

static void test_audio_bar0_resolves_to_usable_mmio_base(void)
{
    const pci_device_t *hda = find_audio();
    CU_ASSERT_PTR_NOT_NULL(hda);
    if (hda == NULL) {
        return;
    }

    pci_bar_t bar;
    CU_ASSERT_EQUAL(pci_bar_read(hda, 0, &bar), PCI_OK);

    /* HDA's register file is memory-mapped, never port I/O. */
    CU_ASSERT_EQUAL(bar.is_io, 0);

    /* Sizing must produce a power-of-two size big enough for the HDA
     * register set (the spec's mandatory block runs past 0x2000), and
     * firmware must have left the BAR programmed at a natural alignment
     * above RAM. */
    CU_ASSERT(bar.size >= 0x1000);
    CU_ASSERT_EQUAL(bar.size & (bar.size - 1), 0);
    CU_ASSERT_NOT_EQUAL(bar.base, 0);
    CU_ASSERT_EQUAL(bar.base & (bar.size - 1), 0);

    /* "Usable" means dereferenceable: vmm_init() maps RAM and the
     * framebuffer aperture only, so the BAR is on unmapped memory until
     * pci_bar_map() puts it in the kernel map. */
    CU_ASSERT_EQUAL(pci_bar_map(&bar), PCI_OK);

    uint64_t paddr = 0, flags = 0;
    CU_ASSERT_EQUAL(vmm_translate(bar.base, &paddr, &flags), VMM_OK);
    CU_ASSERT_EQUAL(paddr, bar.base);           /* identity mapping */
    CU_ASSERT(flags & VMM_PRESENT);
    CU_ASSERT(flags & VMM_WRITE);
}

static void test_bar_sizing_restores_the_original_value(void)
{
    const pci_device_t *hda = find_audio();
    CU_ASSERT_PTR_NOT_NULL(hda);
    if (hda == NULL) {
        return;
    }

    /* Sizing writes all-ones into the BAR. If the restore or the command-
     * register save/restore around it were wrong, the device would be left
     * decoding a garbage window -- so read the raw register and the command
     * register before and after, not just the decoded struct. */
    uint32_t bar_before = pci_config_read32(hda->bus, hda->device,
                                            hda->function, PCI_OFF_BAR0);
    uint16_t cmd_before = pci_config_read16(hda->bus, hda->device,
                                            hda->function, PCI_OFF_COMMAND);

    pci_bar_t bar;
    CU_ASSERT_EQUAL(pci_bar_read(hda, 0, &bar), PCI_OK);

    CU_ASSERT_EQUAL(pci_config_read32(hda->bus, hda->device, hda->function,
                                      PCI_OFF_BAR0), bar_before);
    CU_ASSERT_EQUAL(pci_config_read16(hda->bus, hda->device, hda->function,
                                      PCI_OFF_COMMAND), cmd_before);
}

static void test_bar_read_rejects_bad_arguments(void)
{
    const pci_device_t *hda = find_audio();
    CU_ASSERT_PTR_NOT_NULL(hda);
    if (hda == NULL) {
        return;
    }

    pci_bar_t bar;
    CU_ASSERT_EQUAL(pci_bar_read(hda, -1, &bar), PCI_EINVAL);
    CU_ASSERT_EQUAL(pci_bar_read(hda, PCI_MAX_BARS, &bar), PCI_EINVAL);
    CU_ASSERT_EQUAL(pci_bar_read(hda, 0, NULL), PCI_EINVAL);
    CU_ASSERT_EQUAL(pci_bar_read(NULL, 0, &bar), PCI_EINVAL);

    /* HDA implements BAR0 (and, on some models, its 64-bit upper half)
     * and nothing at BAR4/BAR5: an unimplemented BAR reports ENODEV with
     * a zero size rather than a bogus window. */
    CU_ASSERT_EQUAL(pci_bar_read(hda, 5, &bar), PCI_ENODEV);
    CU_ASSERT_EQUAL(bar.size, 0);
}

static void test_bar_map_rejects_io_and_unassigned(void)
{
    pci_bar_t io_bar = { .base = 0xC000, .size = 0x40, .is_io = 1,
                         .is_64 = 0, .prefetchable = 0 };
    CU_ASSERT_EQUAL(pci_bar_map(&io_bar), PCI_EINVAL);

    pci_bar_t unassigned = { .base = 0, .size = 0x1000, .is_io = 0,
                             .is_64 = 0, .prefetchable = 0 };
    CU_ASSERT_EQUAL(pci_bar_map(&unassigned), PCI_EINVAL);

    CU_ASSERT_EQUAL(pci_bar_map(NULL), PCI_EINVAL);
}

static void test_enable_device_sets_decoders(void)
{
    const pci_device_t *hda = find_audio();
    CU_ASSERT_PTR_NOT_NULL(hda);
    if (hda == NULL) {
        return;
    }

    uint16_t before = pci_config_read16(hda->bus, hda->device, hda->function,
                                        PCI_OFF_COMMAND);

    CU_ASSERT_EQUAL(pci_enable_device(hda), PCI_OK);

    uint16_t after = pci_config_read16(hda->bus, hda->device, hda->function,
                                       PCI_OFF_COMMAND);
    CU_ASSERT(after & PCI_CMD_BUS_MASTER);
    CU_ASSERT(after & PCI_CMD_MEM_SPACE);   /* HDA's BAR0 is memory space */

    /* Only ever sets bits: whatever firmware had on stays on. */
    CU_ASSERT_EQUAL(after & before, before);

    CU_ASSERT_EQUAL(pci_enable_device(NULL), PCI_EINVAL);
}

void suite_pci_tests(CU_pSuite s)
{
    CU_add_test(s, "host bridge answers config reads",
                test_host_bridge_is_present);
    CU_add_test(s, "absent function reads back all ones",
                test_absent_function_reads_all_ones);
    CU_add_test(s, "8/16-bit reads agree with the 32-bit access",
                test_read16_halves_match_read32);
    CU_add_test(s, "enumeration finds the machine's devices",
                test_enumeration_finds_devices);
    CU_add_test(s, "no function is recorded twice",
                test_no_duplicate_functions_recorded);
    CU_add_test(s, "host bridge is recorded with the right class",
                test_host_bridge_recorded_with_right_class);
    CU_add_test(s, "finds the intel-hda audio controller (class 0x0403)",
                test_finds_audio_controller);
    CU_add_test(s, "audio BAR0 resolves to a usable MMIO base",
                test_audio_bar0_resolves_to_usable_mmio_base);
    CU_add_test(s, "BAR sizing restores the register and command bits",
                test_bar_sizing_restores_the_original_value);
    CU_add_test(s, "pci_bar_read rejects bad arguments",
                test_bar_read_rejects_bad_arguments);
    CU_add_test(s, "pci_bar_map rejects I/O and unassigned BARs",
                test_bar_map_rejects_io_and_unassigned);
    CU_add_test(s, "pci_enable_device sets bus master + memory space",
                test_enable_device_sets_decoders);
}
