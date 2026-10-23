/*
 * test_usb_kbd_k.c — HID boot report decoding (SCRUM-198).
 *
 * Drives usb_kbd_process_report() on a test slot that has no device behind
 * it (usb_kbd_test_attach()), so nothing here depends on QEMU's keyboards.
 * The live devices are test_usb_k.c's job. Each test attaches its own slot
 * and detaches it before returning, so the two real keyboards' indices and
 * usb_kbd_count() are what they were.
 */

#include "kunit.h"
#include "usb_kbd.h"
#include "kbd_ring.h"
#include "ps2.h"

#include <stdint.h>

/* HID usages used below. */
#define U_A     0x04
#define U_B     0x05
#define U_1     0x1E
#define U_0     0x27
#define U_ENTER 0x28
#define U_ESC   0x29
#define U_F1    0x3A
#define U_F12   0x45
#define U_RIGHT 0x4F
#define U_UP    0x52
#define U_CAPS  0x39       /* no ps2_key_t for it */

#define M_LCTRL  0x01
#define M_LSHIFT 0x02
#define M_RALT   0x40
#define M_LGUI   0x08

static int g_slot = -1;

static void attach(void)
{
    g_slot = usb_kbd_test_attach();
    CU_ASSERT(g_slot >= 0);
}

static void detach(void)
{
    if (g_slot >= 0) {
        usb_kbd_test_detach(g_slot);
        g_slot = -1;
    }
}

static void report(uint8_t mods, uint8_t k0, uint8_t k1)
{
    uint8_t r[USB_KBD_REPORT_SIZE] = { mods, 0, k0, k1, 0, 0, 0, 0 };
    usb_kbd_process_report(g_slot, r, sizeof(r));
}

static int pop(kbd_event_t *ev)
{
    return usb_kbd_pop(g_slot, ev);
}

static void test_usage_mapping(void)
{
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_A), KEY_A);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(0x1D), KEY_Z);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_1), KEY_1);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(0x26), KEY_9);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_0), KEY_0);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_ENTER), KEY_ENTER);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_ESC), KEY_ESC);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_F1), KEY_F1);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_F12), KEY_F12);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_RIGHT), KEY_RIGHT);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_UP), KEY_UP);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(U_CAPS), KEY_UNKNOWN);
    CU_ASSERT_EQUAL(usb_kbd_usage_to_key(0x00), KEY_UNKNOWN);
}

static void test_press_then_release(void)
{
    kbd_event_t ev;

    attach();
    report(0, U_A, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 1);
    CU_ASSERT_FALSE(pop(&ev));

    report(0, 0, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 0);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_two_keys_in_one_report(void)
{
    kbd_event_t ev1, ev2, ev;

    attach();
    report(0, U_A, U_B);
    CU_ASSERT(pop(&ev1));
    CU_ASSERT(pop(&ev2));
    CU_ASSERT_FALSE(pop(&ev));
    CU_ASSERT_EQUAL(ev1.key, KEY_A);
    CU_ASSERT_EQUAL(ev2.key, KEY_B);
    CU_ASSERT(ev1.pressed && ev2.pressed);

    /* A stays down, B goes up: only B's release. */
    report(0, U_A, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_B);
    CU_ASSERT_EQUAL(ev.pressed, 0);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_key_moves_slot(void)
{
    kbd_event_t ev;

    /* The same key reported in a different array slot is not a change. */
    attach();
    report(0, U_A, U_B);
    while (pop(&ev)) {
    }
    report(0, U_B, U_A);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_repeat_report_is_silent(void)
{
    kbd_event_t ev;

    attach();
    report(0, U_ENTER, 0);
    CU_ASSERT(pop(&ev));
    report(0, U_ENTER, 0);
    report(0, U_ENTER, 0);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_modifiers(void)
{
    kbd_event_t ev;

    attach();
    report(M_LSHIFT, 0, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_SHIFT_LEFT);
    CU_ASSERT_EQUAL(ev.pressed, 1);
    /* Like a PS/2 shift, the mask already includes the key's own bit. */
    CU_ASSERT_EQUAL(ev.modifiers, MOD_LSHIFT);

    report(M_LSHIFT, U_A, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.modifiers, MOD_LSHIFT);

    report(0, U_A, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_SHIFT_LEFT);
    CU_ASSERT_EQUAL(ev.pressed, 0);
    CU_ASSERT_EQUAL(ev.modifiers, 0);

    /* Left Ctrl and right Alt map to the shared keys but distinct bits. */
    report(M_LCTRL | M_RALT, U_A, 0);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_CTRL);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_ALT);
    CU_ASSERT_EQUAL(ev.modifiers, MOD_LCTRL | MOD_RALT);

    /* GUI has no ps2_key_t: no event, no mask change. */
    report(M_LCTRL | M_RALT | M_LGUI, U_A, 0);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_rollover_ignored(void)
{
    kbd_event_t ev;

    attach();
    report(0, U_A, 0);
    CU_ASSERT(pop(&ev));

    /* ErrorRollOver in every slot: state unknown, not "all released". */
    uint8_t err[USB_KBD_REPORT_SIZE] = { 0, 0, 1, 1, 1, 1, 1, 1 };
    usb_kbd_process_report(g_slot, err, sizeof(err));
    CU_ASSERT_FALSE(pop(&ev));

    /* And the next good report is diffed against the pre-rollover one. */
    report(0, U_A, U_B);
    CU_ASSERT(pop(&ev));
    CU_ASSERT_EQUAL(ev.key, KEY_B);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_unmapped_usage_is_silent(void)
{
    kbd_event_t ev;

    attach();
    report(0, U_CAPS, 0);
    CU_ASSERT_FALSE(pop(&ev));
    report(0, 0, 0);
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_short_report_ignored(void)
{
    kbd_event_t ev;
    uint8_t r[2] = { M_LSHIFT, 0 };

    attach();
    usb_kbd_process_report(g_slot, r, sizeof(r));
    CU_ASSERT_FALSE(pop(&ev));
    detach();
}

static void test_full_ring_counts_drops(void)
{
    kbd_event_t ev;

    attach();
    /* Each round trip is two events; the ring holds CAPACITY - 1. */
    for (uint32_t i = 0; i < KBD_RING_CAPACITY; i++) {
        report(0, U_A, 0);
        report(0, 0, 0);
    }
    CU_ASSERT_EQUAL(usb_kbd_dropped(g_slot), KBD_RING_CAPACITY + 1);

    int n = 0;
    while (pop(&ev)) {
        n++;
    }
    CU_ASSERT_EQUAL(n, (int)(KBD_RING_CAPACITY - 1));
    detach();
}

static void test_slots_are_independent(void)
{
    kbd_event_t ev;
    int a = usb_kbd_test_attach();
    int b = usb_kbd_test_attach();

    CU_ASSERT(a >= 0 && b >= 0 && a != b);
    if (a >= 0 && b >= 0) {
        uint8_t ra[USB_KBD_REPORT_SIZE] = { 0, 0, U_A, 0, 0, 0, 0, 0 };
        usb_kbd_process_report(a, ra, sizeof(ra));
        CU_ASSERT_FALSE(usb_kbd_pop(b, &ev));
        CU_ASSERT(usb_kbd_pop(a, &ev));
        CU_ASSERT_EQUAL(ev.key, KEY_A);
    }
    if (b >= 0) {
        usb_kbd_test_detach(b);
    }
    if (a >= 0) {
        usb_kbd_test_detach(a);
    }
}

void suite_usb_kbd_report_tests(CU_pSuite s)
{
    CU_add_test(s, "HID usages map to ps2_key_t", test_usage_mapping);
    CU_add_test(s, "press then release", test_press_then_release);
    CU_add_test(s, "two keys in one report", test_two_keys_in_one_report);
    CU_add_test(s, "a key moving array slot is no change",
                test_key_moves_slot);
    CU_add_test(s, "an identical report is silent",
                test_repeat_report_is_silent);
    CU_add_test(s, "modifier byte becomes key events and MOD_* bits",
                test_modifiers);
    CU_add_test(s, "ErrorRollOver reports are ignored",
                test_rollover_ignored);
    CU_add_test(s, "unmapped usages queue nothing",
                test_unmapped_usage_is_silent);
    CU_add_test(s, "a report shorter than 3 bytes is ignored",
                test_short_report_ignored);
    CU_add_test(s, "a full ring counts drops", test_full_ring_counts_drops);
    CU_add_test(s, "keyboard slots have separate rings",
                test_slots_are_independent);
}
