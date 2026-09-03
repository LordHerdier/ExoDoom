/*
 * test_kbd_ring.c — Kernel-side CUnit tests for the keyboard event ring
 *                   (SCRUM-18) and the scancode decoder that feeds it.
 *
 * Acceptance criteria under test:
 *   - rapid typing does not drop keys
 *   - buffer overflow is handled gracefully (drop, no corruption, no crash)
 *   - both press and release events are queued, including extended keys
 *
 * These run before interrupts are enabled, so driving ps2_process_scancode
 * directly is race-free and needs no hardware.
 */

#include "kunit.h"
#include "kbd_ring.h"
#include "ps2.h"

/* 768 bytes of events — kept off the 16 KB kernel stack. */
static kbd_ring_t ring;

static kbd_event_t make_event(uint8_t key, uint8_t pressed, uint8_t mods)
{
    kbd_event_t ev;
    ev.key       = key;
    ev.pressed   = pressed;
    ev.modifiers = mods;
    return ev;
}

/* ---- Ring buffer ------------------------------------------------------- */

static void test_init_is_empty(void)
{
    kbd_event_t ev;

    kbd_ring_init(&ring);

    CU_ASSERT_TRUE(kbd_ring_is_empty(&ring));
    CU_ASSERT_FALSE(kbd_ring_is_full(&ring));
    CU_ASSERT_EQUAL(kbd_ring_count(&ring), 0);
    CU_ASSERT_EQUAL(kbd_ring_dropped(&ring), 0);
    CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 0);
}

static void test_fifo_order_preserved(void)
{
    kbd_event_t ev;

    kbd_ring_init(&ring);

    for (uint8_t i = 0; i < 8; i++)
        CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event(i, i & 1, i)), 1);

    CU_ASSERT_EQUAL(kbd_ring_count(&ring), 8);

    for (uint8_t i = 0; i < 8; i++) {
        CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 1);
        CU_ASSERT_EQUAL(ev.key, i);
        CU_ASSERT_EQUAL(ev.pressed, i & 1);
        CU_ASSERT_EQUAL(ev.modifiers, i);
    }

    CU_ASSERT_TRUE(kbd_ring_is_empty(&ring));
}

static void test_pop_empty_leaves_output_untouched(void)
{
    kbd_event_t ev = make_event(0xAB, 1, 0xCD);

    kbd_ring_init(&ring);

    CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 0);
    CU_ASSERT_EQUAL(ev.key, 0xAB);
    CU_ASSERT_EQUAL(ev.modifiers, 0xCD);
}

/* Push and pop far past the capacity to prove the index wrap is correct —
 * a long typing session must not corrupt or reorder events. */
static void test_wraparound_beyond_capacity(void)
{
    kbd_event_t ev;

    kbd_ring_init(&ring);

    for (unsigned i = 0; i < KBD_RING_CAPACITY * 3; i++) {
        CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event((uint8_t)i, i & 1, 0)), 1);
        CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 1);
        CU_ASSERT_EQUAL(ev.key, (uint8_t)i);
        CU_ASSERT_EQUAL(ev.pressed, i & 1);
    }

    CU_ASSERT_TRUE(kbd_ring_is_empty(&ring));
    CU_ASSERT_EQUAL(kbd_ring_dropped(&ring), 0);
}

/* Overflow policy: the newest event is dropped, everything already queued
 * survives intact and in order, and the drop is counted. */
static void test_overflow_drops_gracefully(void)
{
    const unsigned usable = KBD_RING_CAPACITY - 1;
    kbd_event_t ev;

    kbd_ring_init(&ring);

    for (unsigned i = 0; i < usable; i++)
        CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event((uint8_t)i, 1, 0)), 1);

    CU_ASSERT_TRUE(kbd_ring_is_full(&ring));
    CU_ASSERT_EQUAL(kbd_ring_count(&ring), usable);

    /* Three pushes against a full ring */
    CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event(0xFF, 1, 0)), 0);
    CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event(0xFF, 1, 0)), 0);
    CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event(0xFF, 1, 0)), 0);

    CU_ASSERT_EQUAL(kbd_ring_dropped(&ring), 3);
    CU_ASSERT_EQUAL(kbd_ring_count(&ring), usable);

    /* Everything queued before the overflow is still there, in order. */
    for (unsigned i = 0; i < usable; i++) {
        CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 1);
        CU_ASSERT_EQUAL(ev.key, (uint8_t)i);
    }

    CU_ASSERT_TRUE(kbd_ring_is_empty(&ring));
}

/* The ring recovers as soon as the consumer catches up. */
static void test_drains_and_accepts_again_after_overflow(void)
{
    kbd_event_t ev;

    kbd_ring_init(&ring);

    for (unsigned i = 0; i < KBD_RING_CAPACITY; i++)
        kbd_ring_push(&ring, make_event((uint8_t)i, 1, 0));

    CU_ASSERT_EQUAL(kbd_ring_dropped(&ring), 1);

    while (kbd_ring_pop(&ring, &ev))
        ;

    CU_ASSERT_EQUAL(kbd_ring_push(&ring, make_event(0x42, 0, 0)), 1);
    CU_ASSERT_EQUAL(kbd_ring_pop(&ring, &ev), 1);
    CU_ASSERT_EQUAL(ev.key, 0x42);
}

static void test_null_arguments_are_safe(void)
{
    kbd_event_t ev;

    kbd_ring_init(&ring);

    CU_ASSERT_EQUAL(kbd_ring_push(NULL, make_event(1, 1, 0)), 0);
    CU_ASSERT_EQUAL(kbd_ring_pop(NULL, &ev), 0);
    CU_ASSERT_EQUAL(kbd_ring_pop(&ring, NULL), 0);
    CU_ASSERT_EQUAL(kbd_ring_count(NULL), 0);
    CU_ASSERT_TRUE(kbd_ring_is_empty(NULL));
}

void suite_kbd_ring_tests(CU_pSuite s)
{
    CU_add_test(s, "init_is_empty",             test_init_is_empty);
    CU_add_test(s, "fifo_order_preserved",      test_fifo_order_preserved);
    CU_add_test(s, "pop_empty_untouched",       test_pop_empty_leaves_output_untouched);
    CU_add_test(s, "wraparound",                test_wraparound_beyond_capacity);
    CU_add_test(s, "overflow_drops_gracefully", test_overflow_drops_gracefully);
    CU_add_test(s, "recovers_after_overflow",   test_drains_and_accepts_again_after_overflow);
    CU_add_test(s, "null_arguments_safe",       test_null_arguments_are_safe);
}

/* ---- Scancode decoder -> queue ----------------------------------------- */

static void feed(const uint8_t *bytes, unsigned count)
{
    for (unsigned i = 0; i < count; i++)
        ps2_process_scancode(bytes[i]);
}

static void test_press_then_release(void)
{
    static const uint8_t stream[] = { 0x1E, 0x9E };   /* A make, A break */
    kbd_event_t ev;

    kbd_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(kbd_pending(), 2);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 1);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 0);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 0);
}

/* Extended keys break as E0 <make|0x80>; the release must reach the queue. */
static void test_extended_press_then_release(void)
{
    static const uint8_t stream[] = { 0xE0, 0x48, 0xE0, 0xC8 };  /* Up */
    kbd_event_t ev;

    kbd_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(kbd_pending(), 2);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_UP);
    CU_ASSERT_EQUAL(ev.pressed, 1);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_UP);
    CU_ASSERT_EQUAL(ev.pressed, 0);
}

static void test_modifier_state_travels_with_event(void)
{
    static const uint8_t stream[] = { 0x2A, 0x1E, 0x9E, 0xAA };
    /* LShift down, A down, A up, LShift up */
    kbd_event_t ev;

    kbd_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(kbd_pending(), 4);

    kbd_dequeue(&ev);                                  /* LShift down */
    CU_ASSERT_EQUAL(ev.key, KEY_SHIFT_LEFT);
    CU_ASSERT_TRUE(ev.modifiers & MOD_LSHIFT);

    kbd_dequeue(&ev);                                  /* A down */
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_TRUE(ev.modifiers & MOD_LSHIFT);

    kbd_dequeue(&ev);                                  /* A up */
    CU_ASSERT_TRUE(ev.modifiers & MOD_LSHIFT);

    kbd_dequeue(&ev);                                  /* LShift up */
    CU_ASSERT_FALSE(ev.modifiers & MOD_LSHIFT);
    CU_ASSERT_EQUAL(ps2_get_modifier_state(), 0);
}

/* Right Ctrl is E0 1D and must set MOD_RCTRL, not MOD_LCTRL. */
static void test_right_ctrl_uses_right_modifier_bit(void)
{
    static const uint8_t stream[] = { 0xE0, 0x1D, 0xE0, 0x9D };
    kbd_event_t ev;

    kbd_reset();
    feed(stream, sizeof stream);

    kbd_dequeue(&ev);
    CU_ASSERT_EQUAL(ev.key, KEY_CTRL);
    CU_ASSERT_TRUE(ev.modifiers & MOD_RCTRL);
    CU_ASSERT_FALSE(ev.modifiers & MOD_LCTRL);

    kbd_dequeue(&ev);
    CU_ASSERT_EQUAL(ev.pressed, 0);
    CU_ASSERT_EQUAL(ps2_get_modifier_state(), 0);
}

static void test_unmapped_scancodes_queue_nothing(void)
{
    static const uint8_t stream[] = { 0x00, 0x59, 0x5A, 0x7F };

    kbd_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(kbd_pending(), 0);
}

/* Pause/Break is E1 1D 45 E1 9D C5; decoding 1D would fake a Ctrl press. */
static void test_pause_sequence_queues_nothing(void)
{
    static const uint8_t stream[] = { 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5 };

    kbd_reset();
    feed(stream, sizeof stream);

    CU_ASSERT_EQUAL(kbd_pending(), 0);
    CU_ASSERT_EQUAL(ps2_get_modifier_state(), 0);
}

/*
 * 0xF0 is a set 2 break prefix, but the controller gives us translated set 1,
 * where it is the ordinary break code of 0x70 (kana).  It must not be treated
 * as a prefix: doing so turned the *next* key's press into a release.
 */
static void test_set2_break_prefix_does_not_invert_next_key(void)
{
    static const uint8_t stream[] = { 0xF0, 0x1E };   /* kana break, A make */
    kbd_event_t ev;

    kbd_reset();
    feed(stream, sizeof stream);

    /* 0x70 is unmapped, so the kana release queues nothing; A must still
     * arrive as a press. */
    CU_ASSERT_EQUAL(kbd_pending(), 1);
    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 1);
}

/*
 * Modifier queries read the per-key mask, so releasing one key of a pair while
 * the other is held must not clear the state.
 */
static void test_modifier_active_while_paired_key_held(void)
{
    /* LShift down, RShift down, RShift up */
    static const uint8_t shifts[] = { 0x2A, 0x36, 0xB6 };
    /* LCtrl down, RCtrl (E0 1D) down, RCtrl up */
    static const uint8_t ctrls[] = { 0x1D, 0xE0, 0x1D, 0xE0, 0x9D };

    kbd_reset();
    feed(shifts, sizeof shifts);

    CU_ASSERT_TRUE(ps2_shift_active());
    CU_ASSERT_EQUAL(ps2_get_modifier_state() & MOD_LSHIFT, MOD_LSHIFT);
    CU_ASSERT_EQUAL(ps2_get_modifier_state() & MOD_RSHIFT, 0);

    ps2_process_scancode(0xAA);                      /* LShift up */
    CU_ASSERT_FALSE(ps2_shift_active());

    kbd_reset();
    feed(ctrls, sizeof ctrls);

    CU_ASSERT_TRUE(ps2_ctrl_active());
    CU_ASSERT_EQUAL(ps2_get_modifier_state() & MOD_LCTRL, MOD_LCTRL);
    CU_ASSERT_EQUAL(ps2_get_modifier_state() & MOD_RCTRL, 0);
}

/* kbd_enqueue is driver API: an injected event keeps the modifiers it was
 * built with rather than having the live IRQ-side state stamped over them. */
static void test_enqueue_preserves_caller_modifiers(void)
{
    kbd_event_t ev;

    kbd_reset();
    ps2_process_scancode(0x2A);                      /* LShift down */
    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);            /* drain the shift event */

    kbd_event_t injected = {
        .pressed   = 1,
        .key       = (uint8_t)KEY_B,
        .modifiers = MOD_RALT,
    };

    kbd_enqueue(injected);

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_B);
    CU_ASSERT_EQUAL(ev.modifiers, MOD_RALT);
}

/*
 * The acceptance criterion itself: a burst far larger than any human can type
 * between two polls arrives complete and in order.
 */
static void test_rapid_burst_drops_nothing(void)
{
    const unsigned keystrokes = 100;   /* 200 events, queue holds 255 */
    kbd_event_t ev;

    kbd_reset();

    for (unsigned i = 0; i < keystrokes; i++) {
        ps2_process_scancode(0x1E);        /* A down */
        ps2_process_scancode(0x9E);        /* A up   */
    }

    CU_ASSERT_EQUAL(kbd_pending(), keystrokes * 2);
    CU_ASSERT_EQUAL(kbd_dropped_count(), 0);

    for (unsigned i = 0; i < keystrokes; i++) {
        CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
        CU_ASSERT_EQUAL(ev.key, KEY_A);
        CU_ASSERT_EQUAL(ev.pressed, 1);

        CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
        CU_ASSERT_EQUAL(ev.key, KEY_A);
        CU_ASSERT_EQUAL(ev.pressed, 0);
    }

    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 0);
}

/* A consumer that never polls must not take the driver down with it. */
static void test_unserviced_queue_overflows_gracefully(void)
{
    const unsigned events = KBD_RING_CAPACITY * 2;
    kbd_event_t ev;

    kbd_reset();

    for (unsigned i = 0; i < events; i++)
        ps2_process_scancode(0x1E);

    CU_ASSERT_EQUAL(kbd_pending(), KBD_RING_CAPACITY - 1);
    CU_ASSERT_EQUAL(kbd_dropped_count(), events - (KBD_RING_CAPACITY - 1));

    /* Queued events are still well formed after the overflow. */
    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_A);
    CU_ASSERT_EQUAL(ev.pressed, 1);

    /* And the driver keeps working once drained. */
    kbd_reset();
    ps2_process_scancode(0x39);            /* Space down */
    CU_ASSERT_EQUAL(kbd_dropped_count(), 0);
    CU_ASSERT_EQUAL(kbd_dequeue(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_SPACE);
}

static void test_exo_kbd_poll_matches_dequeue(void)
{
    kbd_event_t ev;

    kbd_reset();
    ps2_process_scancode(0x1C);            /* Enter down */

    CU_ASSERT_EQUAL(exo_kbd_poll(NULL), 0);
    CU_ASSERT_EQUAL(exo_kbd_poll(&ev), 1);
    CU_ASSERT_EQUAL(ev.key, KEY_ENTER);
    CU_ASSERT_EQUAL(ev.pressed, 1);
    CU_ASSERT_EQUAL(exo_kbd_poll(&ev), 0);
}

void suite_ps2_decode_tests(CU_pSuite s)
{
    CU_add_test(s, "press_then_release",        test_press_then_release);
    CU_add_test(s, "extended_press_release",    test_extended_press_then_release);
    CU_add_test(s, "modifiers_in_event",        test_modifier_state_travels_with_event);
    CU_add_test(s, "right_ctrl_modifier",       test_right_ctrl_uses_right_modifier_bit);
    CU_add_test(s, "unmapped_queue_nothing",    test_unmapped_scancodes_queue_nothing);
    CU_add_test(s, "pause_queues_nothing",      test_pause_sequence_queues_nothing);
    CU_add_test(s, "set2_prefix_not_a_prefix",  test_set2_break_prefix_does_not_invert_next_key);
    CU_add_test(s, "paired_modifier_held",      test_modifier_active_while_paired_key_held);
    CU_add_test(s, "enqueue_keeps_modifiers",   test_enqueue_preserves_caller_modifiers);
    CU_add_test(s, "rapid_burst_no_drop",       test_rapid_burst_drops_nothing);
    CU_add_test(s, "overflow_graceful",         test_unserviced_queue_overflows_gracefully);
    CU_add_test(s, "exo_kbd_poll",              test_exo_kbd_poll_matches_dequeue);
}
