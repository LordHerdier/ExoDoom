/*
 * test_ps2_mouse_k.c — Kernel-side CUnit tests for the PS/2 mouse packet
 *                      decoder and accumulator (SCRUM-52).
 *
 * Acceptance criteria under test:
 *   - all four sign-bit quadrant combinations decode to the right dx/dy
 *   - an overflow bit drops that axis's delta to 0 rather than applying
 *     garbage
 *   - a misaligned byte 0 (bit 3 clear) is dropped and the decoder resyncs
 *     on the next byte that does carry the alignment bit
 *   - deltas accumulate across multiple packets before a poll
 *   - a poll resets dx/dy to 0 but leaves buttons as a level
 *   - repeated large deltas saturate at INT16_MIN/INT16_MAX rather than
 *     wrapping
 *
 * These run before interrupts are enabled, so driving
 * ps2_mouse_process_byte() directly is race-free and needs no hardware --
 * same reasoning as test_kbd_ring.c's scancode-decoder half.
 */

#include "kunit.h"
#include "ps2_mouse.h"

#include <stdint.h>

static void feed(const uint8_t *bytes, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        ps2_mouse_process_byte(bytes[i]);
}

/* Byte 0 layout: bit3 always 1, bit4 X sign, bit5 Y sign, bit6 X overflow,
 * bit7 Y overflow, bits0-2 buttons. */
static uint8_t byte0(uint8_t buttons, int x_sign, int y_sign,
                     int x_overflow, int y_overflow)
{
    uint8_t b = 0x08 | (buttons & 0x07);
    if (x_sign)     b |= 0x10;
    if (y_sign)     b |= 0x20;
    if (x_overflow) b |= 0x40;
    if (y_overflow) b |= 0x80;
    return b;
}

static void test_positive_dx_positive_dy(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 0, 0, 0), 10, 20 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 10);
    CU_ASSERT_EQUAL(st.dy, 20);
    CU_ASSERT_EQUAL(st.buttons, 0);
}

static void test_negative_dx_positive_dy(void)
{
    ps2_mouse_state_t st;
    /* -10 as an 8-bit two's complement magnitude byte is 0xF6 (246). */
    uint8_t packet[] = { byte0(0, 1, 0, 0, 0), 0xF6, 20 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, -10);
    CU_ASSERT_EQUAL(st.dy, 20);
}

static void test_positive_dx_negative_dy(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 1, 0, 0), 10, 0xEC /* -20 */ };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 10);
    CU_ASSERT_EQUAL(st.dy, -20);
}

static void test_negative_dx_negative_dy(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 1, 1, 0, 0), 0xF6 /* -10 */, 0xEC /* -20 */ };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, -10);
    CU_ASSERT_EQUAL(st.dy, -20);
}

/* Overflow means the reported movement for that axis is garbage -- it must
 * be dropped to 0 rather than applied. */
static void test_x_overflow_drops_x_only(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 0, 1, 0), 100, 5 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 0);
    CU_ASSERT_EQUAL(st.dy, 5);
}

static void test_y_overflow_drops_y_only(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 0, 0, 1), 5, 100 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 5);
    CU_ASSERT_EQUAL(st.dy, 0);
}

/* Buttons are a level, not a delta: whatever byte 0 says is what's live. */
static void test_buttons_reported_as_level(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0x05 /* left + middle */, 0, 0, 0, 0), 0, 0 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.buttons, 0x05);
}

/* Byte 0's alignment bit (bit 3) must be set; a stray byte without it is
 * dropped and the decoder resyncs on the next properly-aligned byte rather
 * than building a packet out of the wrong three bytes. */
static void test_misaligned_byte_resyncs(void)
{
    ps2_mouse_state_t st;
    /* 0x00 has bit 3 clear -- garbage, dropped. Then a real packet. */
    uint8_t stream[] = { 0x00, byte0(0, 0, 0, 0, 0), 7, 3 };

    ps2_mouse_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(ps2_mouse_resync_count(), 1);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 7);
    CU_ASSERT_EQUAL(st.dy, 3);
}

/* Two packets between polls accumulate rather than overwrite. */
static void test_deltas_accumulate_across_packets(void)
{
    ps2_mouse_state_t st;
    uint8_t p1[] = { byte0(0, 0, 0, 0, 0), 5, 5 };
    uint8_t p2[] = { byte0(0, 0, 0, 0, 0), 3, 4 };

    ps2_mouse_reset();
    feed(p1, sizeof p1);
    feed(p2, sizeof p2);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 8);
    CU_ASSERT_EQUAL(st.dy, 9);
}

/* A poll resets dx/dy to zero but buttons persists as a level until the next
 * packet changes it -- matches exo_mouse_poll's documented contract. */
static void test_poll_resets_deltas_not_buttons(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0x01, 0, 0, 0, 0), 1, 1 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 1);
    CU_ASSERT_EQUAL(st.buttons, 0x01);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 0);
    CU_ASSERT_EQUAL(st.dy, 0);
    CU_ASSERT_EQUAL(st.buttons, 0x01);
}

/* Enough large packets between polls to overflow an int16_t accumulator must
 * saturate, not wrap into a bogus small or negative value. */
static void test_accumulator_saturates_at_int16_max(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 0, 0, 0), 127, 0 };

    ps2_mouse_reset();
    for (int i = 0; i < 400; i++)   /* 400 * 127 far exceeds INT16_MAX */
        feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, INT16_MAX);
}

static void test_accumulator_saturates_at_int16_min(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 1, 0, 0, 0), 0x80 /* -128 */, 0 };

    ps2_mouse_reset();
    for (int i = 0; i < 400; i++)
        feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, INT16_MIN);
}

/* A byte fed mid-packet (position 1 or 2) is never checked against the
 * alignment bit -- only byte 0 carries it -- so an ordinary packet whose
 * movement bytes happen to have bit 3 set must still decode normally. */
static void test_movement_byte_alignment_bit_irrelevant(void)
{
    ps2_mouse_state_t st;
    uint8_t packet[] = { byte0(0, 0, 0, 0, 0), 0x08, 0x08 };

    ps2_mouse_reset();
    feed(packet, sizeof packet);

    ps2_mouse_poll(&st);
    CU_ASSERT_EQUAL(st.dx, 8);
    CU_ASSERT_EQUAL(st.dy, 8);
}

void suite_ps2_mouse_tests(CU_pSuite s)
{
    CU_add_test(s, "positive_dx_positive_dy",  test_positive_dx_positive_dy);
    CU_add_test(s, "negative_dx_positive_dy",  test_negative_dx_positive_dy);
    CU_add_test(s, "positive_dx_negative_dy",  test_positive_dx_negative_dy);
    CU_add_test(s, "negative_dx_negative_dy",  test_negative_dx_negative_dy);
    CU_add_test(s, "x_overflow_drops_x_only",  test_x_overflow_drops_x_only);
    CU_add_test(s, "y_overflow_drops_y_only",  test_y_overflow_drops_y_only);
    CU_add_test(s, "buttons_reported_as_level", test_buttons_reported_as_level);
    CU_add_test(s, "misaligned_byte_resyncs",  test_misaligned_byte_resyncs);
    CU_add_test(s, "deltas_accumulate",        test_deltas_accumulate_across_packets);
    CU_add_test(s, "poll_resets_deltas_not_buttons",
               test_poll_resets_deltas_not_buttons);
    CU_add_test(s, "saturates_at_int16_max",   test_accumulator_saturates_at_int16_max);
    CU_add_test(s, "saturates_at_int16_min",   test_accumulator_saturates_at_int16_min);
    CU_add_test(s, "movement_byte_alignment_irrelevant",
               test_movement_byte_alignment_bit_irrelevant);
}
