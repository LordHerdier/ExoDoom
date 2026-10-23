/*
 * test_usb_k.c — UHCI bring-up, enumeration and two live USB keyboards
 * (SCRUM-198).
 *
 * Runs against the devices QEMU_USB_TEST_ARGS in the Makefile attaches
 * (docs/drivers/usb.md section 2): three UHCI controllers, two usb-kbd on
 * the first one's ports 1 and 2, a usb-mouse and a usb-storage stick. Every
 * assertion is unconditional for the same reason as test_usb_topology_k.c:
 * docker-test/docker-ci always pass that line.
 *
 * The last test is the ticket's acceptance check. docker/scripts/qmp-keys.sh
 * runs next to QEMU for the whole test boot, sending "a" to the console
 * keyboard A is bound to (vga2) and "b" to keyboard B's (vga3), over and
 * over. The test opens a short interrupts-on window so irq0_handler()'s
 * usb_poll() can collect reports, then checks each keyboard saw only its
 * own key -- and that none of it leaked into the PS/2 ring.
 */

#include "kunit.h"
#include "usb.h"
#include "uhci.h"
#include "usb_kbd.h"
#include "ps2.h"
#include "pit.h"
#include "serial.h"

#include <stdint.h>

#define EXPECTED_HCDS    3
#define EXPECTED_DEVICES 4
#define EXPECTED_KBDS    2
#define QEMU_KBD_VENDOR  0x0627
#define QEMU_KBD_PRODUCT 0x0001

/* How long the acceptance test waits for both keys. qmp-keys.sh sends a
 * pair every ~200 ms, so this is generous. */
#define KEY_WAIT_MS 5000

static void test_controllers_registered(void)
{
    CU_ASSERT_EQUAL(usb_hcd_count(), EXPECTED_HCDS);
    CU_ASSERT_EQUAL(uhci_count(), EXPECTED_HCDS);

    for (int i = 0; i < uhci_count(); i++) {
        uhci_info_t info;
        CU_ASSERT_EQUAL(uhci_get_info(i, &info), USB_OK);
        /* QEMU's UHCI has two root ports, found by probing PORTSC. */
        CU_ASSERT_EQUAL(info.ports, 2);
        CU_ASSERT_NOT_EQUAL(info.io_base, 0);
    }
}

static void test_dma_below_4g(void)
{
    for (int i = 0; i < uhci_count(); i++) {
        uhci_info_t info;
        CU_ASSERT_EQUAL(uhci_get_info(i, &info), USB_OK);
        /* UHCI link pointers and FRBASEADD are 32-bit. */
        CU_ASSERT(info.frame_list != 0 && info.frame_list < (1ULL << 32));
        CU_ASSERT(info.sched_page != 0 && info.sched_page < (1ULL << 32));
        CU_ASSERT(info.ctrl_page != 0 && info.ctrl_page < (1ULL << 32));
        CU_ASSERT_EQUAL(info.frame_list & 0xFFF, 0);
        CU_ASSERT_EQUAL(info.sched_page & 0xFFF, 0);
        CU_ASSERT_EQUAL(info.ctrl_page & 0xFFF, 0);
    }
}

static void test_every_device_enumerated(void)
{
    CU_ASSERT_EQUAL(usb_device_count(), EXPECTED_DEVICES);

    for (int i = 0; i < usb_device_count(); i++) {
        const usb_device_t *d = usb_device_at(i);
        CU_ASSERT(d->address >= 1 && d->address <= 127);
        for (int j = 0; j < i; j++) {
            CU_ASSERT_NOT_EQUAL(usb_device_at(j)->address, d->address);
        }
    }
}

static void test_two_keyboards_claimed(void)
{
    int claimed = 0;

    CU_ASSERT_EQUAL(usb_kbd_count(), EXPECTED_KBDS);

    for (int i = 0; i < usb_kbd_count(); i++) {
        usb_kbd_ident_t id;
        CU_ASSERT_EQUAL(usb_kbd_ident(i, &id), USB_OK);
        /* Both on uhci1 (the first controller), on ports 1 and 2. */
        CU_ASSERT_EQUAL(id.hcd, 0);
        CU_ASSERT_EQUAL(id.port, i);
        CU_ASSERT(id.pipe >= 0);
        CU_ASSERT_EQUAL(uhci_pipe_live(id.hcd, id.pipe), 1);
    }

    /* Only the keyboards are claimed: the mouse and the stick wait for
     * SCRUM-246/-247. */
    for (int i = 0; i < usb_device_count(); i++) {
        const usb_device_t *d = usb_device_at(i);
        if (d->driver) {
            claimed++;
            CU_ASSERT_EQUAL(d->vendor_id, QEMU_KBD_VENDOR);
            CU_ASSERT_EQUAL(d->product_id, QEMU_KBD_PRODUCT);
            CU_ASSERT_EQUAL(d->iface_protocol, USB_HID_PROTO_KEYBOARD);
        }
    }
    CU_ASSERT_EQUAL(claimed, EXPECTED_KBDS);
}

typedef struct {
    int a_down, b_down, other;
} seen_t;

static void drain(int idx, seen_t *s)
{
    kbd_event_t ev;

    while (usb_kbd_pop(idx, &ev)) {
        if (!s) {
            continue;
        }
        if (ev.pressed && ev.key == KEY_A) {
            s->a_down++;
        } else if (ev.pressed && ev.key == KEY_B) {
            s->b_down++;
        } else if (ev.key != KEY_A && ev.key != KEY_B) {
            s->other++;
        }
    }
}

static void test_keyboards_are_independent(void)
{
    seen_t k0 = { 0 }, k1 = { 0 };
    kbd_event_t ev;
    int ps2_events = 0;

    if (usb_kbd_count() < EXPECTED_KBDS) {
        CU_ASSERT(0);
        return;
    }

    /* Whatever was collected in earlier suites' interrupts-on windows is
     * stale; start clean. */
    drain(0, NULL);
    drain(1, NULL);
    while (kbd_dequeue(&ev)) {
    }

    /* Local interrupts-on window, as in test_syscall_pit_k.c: usb_poll()
     * only runs from irq0_handler(). */
    uint32_t start = kernel_get_ticks_ms();
    __asm__ volatile ("sti");
    while (kernel_get_ticks_ms() - start < KEY_WAIT_MS
           && !(k0.a_down && k1.b_down)) {
        __asm__ volatile ("hlt");
        __asm__ volatile ("cli");
        drain(0, &k0);
        drain(1, &k1);
        __asm__ volatile ("sti");
    }
    __asm__ volatile ("cli");

    while (kbd_dequeue(&ev)) {
        ps2_events++;
    }

    serial_print("usb_kbd: kbd0 a=");
    serial_print_dec((uint32_t)k0.a_down);
    serial_print(" b=");
    serial_print_dec((uint32_t)k0.b_down);
    serial_print(" | kbd1 a=");
    serial_print_dec((uint32_t)k1.a_down);
    serial_print(" b=");
    serial_print_dec((uint32_t)k1.b_down);
    serial_print(" | ps2 ");
    serial_print_dec((uint32_t)ps2_events);
    serial_print(" (");
    serial_print_dec(kernel_get_ticks_ms() - start);
    serial_print(" ms)\n");

    /* Keyboard A (vga2) types only "a", keyboard B (vga3) only "b". */
    CU_ASSERT(k0.a_down > 0);
    CU_ASSERT_EQUAL(k0.b_down, 0);
    CU_ASSERT(k1.b_down > 0);
    CU_ASSERT_EQUAL(k1.a_down, 0);
    CU_ASSERT_EQUAL(k0.other + k1.other, 0);
    /* Additive: nothing from USB reaches the PS/2 path. */
    CU_ASSERT_EQUAL(ps2_events, 0);
}

void suite_usb_tests(CU_pSuite s)
{
    CU_add_test(s, "every UHCI controller is registered with 2 ports",
                test_controllers_registered);
    CU_add_test(s, "UHCI DMA pages are page-aligned and below 4 GB",
                test_dma_below_4g);
    CU_add_test(s, "every attached device has its own address",
                test_every_device_enumerated);
    CU_add_test(s, "both keyboards claimed with live interrupt pipes",
                test_two_keyboards_claimed);
    CU_add_test(s, "two USB keyboards produce independent key events",
                test_keyboards_are_independent);
}
