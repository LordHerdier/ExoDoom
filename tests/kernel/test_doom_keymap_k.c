/*
 * test_doom_keymap_k.c — ps2_key_t -> Doom keycode translation (SCRUM-40).
 *
 * Two jobs, and the file is laid out in two halves because of a preprocessor
 * hazard src/doom_keymap.h explains at length:
 *
 *   1. The VALUES half proves that doom_keymap.h's restated DOOM_KEY_*
 *      constants still equal the real ones in src/doom/doomkeys.h. This is
 *      what makes restating them safe rather than merely convenient, in the
 *      same spirit as src/doom_panic.c's restated serial cap. It has to
 *      include doomkeys.h.
 *
 *   2. The BEHAVIOUR half drives doom_keymap_translate() over ps2_key_t
 *      values. It has to include src/ps2.h.
 *
 * Those two headers cannot coexist: KEY_ENTER, KEY_TAB, KEY_BACKSPACE,
 * KEY_MINUS, KEY_EQUALS and KEY_F1..KEY_F12 are enum constants in one and
 * object-like macros in the other. So doomkeys.h is included first, the
 * assertions are made immediately, and every colliding macro is #undef'd
 * before ps2.h appears. Those #undefs are not tidying -- without them ps2.h's
 * enum body is rewritten into a syntax error.
 */

#include "kunit.h"
#include "doom_keymap.h"

/* ---- 1. VALUES: the restatement is checked, not trusted ---------------- */

#include "doom/doomkeys.h"

_Static_assert(DOOM_KEY_RIGHTARROW == KEY_RIGHTARROW, "RIGHTARROW drifted");
_Static_assert(DOOM_KEY_LEFTARROW  == KEY_LEFTARROW,  "LEFTARROW drifted");
_Static_assert(DOOM_KEY_UPARROW    == KEY_UPARROW,    "UPARROW drifted");
_Static_assert(DOOM_KEY_DOWNARROW  == KEY_DOWNARROW,  "DOWNARROW drifted");
_Static_assert(DOOM_KEY_STRAFE_L   == KEY_STRAFE_L,   "STRAFE_L drifted");
_Static_assert(DOOM_KEY_STRAFE_R   == KEY_STRAFE_R,   "STRAFE_R drifted");
_Static_assert(DOOM_KEY_USE        == KEY_USE,        "USE drifted");
_Static_assert(DOOM_KEY_FIRE       == KEY_FIRE,       "FIRE drifted");
_Static_assert(DOOM_KEY_ESCAPE     == KEY_ESCAPE,     "ESCAPE drifted");
_Static_assert(DOOM_KEY_ENTER      == KEY_ENTER,      "ENTER drifted");
_Static_assert(DOOM_KEY_TAB        == KEY_TAB,        "TAB drifted");
_Static_assert(DOOM_KEY_BACKSPACE  == KEY_BACKSPACE,  "BACKSPACE drifted");
_Static_assert(DOOM_KEY_PAUSE      == KEY_PAUSE,      "PAUSE drifted");
_Static_assert(DOOM_KEY_EQUALS     == KEY_EQUALS,     "EQUALS drifted");
_Static_assert(DOOM_KEY_MINUS      == KEY_MINUS,      "MINUS drifted");
_Static_assert(DOOM_KEY_RSHIFT     == KEY_RSHIFT,     "RSHIFT drifted");
_Static_assert(DOOM_KEY_RCTRL      == KEY_RCTRL,      "RCTRL drifted");
_Static_assert(DOOM_KEY_RALT       == KEY_RALT,       "RALT drifted");
_Static_assert(DOOM_KEY_F1         == KEY_F1,         "F1 drifted");
_Static_assert(DOOM_KEY_F2         == KEY_F2,         "F2 drifted");
_Static_assert(DOOM_KEY_F3         == KEY_F3,         "F3 drifted");
_Static_assert(DOOM_KEY_F4         == KEY_F4,         "F4 drifted");
_Static_assert(DOOM_KEY_F5         == KEY_F5,         "F5 drifted");
_Static_assert(DOOM_KEY_F6         == KEY_F6,         "F6 drifted");
_Static_assert(DOOM_KEY_F7         == KEY_F7,         "F7 drifted");
_Static_assert(DOOM_KEY_F8         == KEY_F8,         "F8 drifted");
_Static_assert(DOOM_KEY_F9         == KEY_F9,         "F9 drifted");
_Static_assert(DOOM_KEY_F10        == KEY_F10,        "F10 drifted");
_Static_assert(DOOM_KEY_F11        == KEY_F11,        "F11 drifted");
_Static_assert(DOOM_KEY_F12        == KEY_F12,        "F12 drifted");

/* doomkeys.h also confirms the claim doom_keymap.c leans on for ALT. */
_Static_assert(KEY_LALT == KEY_RALT,
               "doomkeys.h no longer aliases LALT to RALT -- doom_keymap.c's "
               "KEY_ALT case now has a side to choose, and needs the modifier "
               "mask back to choose it");

/* Now drop every name that would rewrite ps2.h's enum below. */
#undef KEY_ENTER
#undef KEY_TAB
#undef KEY_BACKSPACE
#undef KEY_MINUS
#undef KEY_EQUALS
#undef KEY_F1
#undef KEY_F2
#undef KEY_F3
#undef KEY_F4
#undef KEY_F5
#undef KEY_F6
#undef KEY_F7
#undef KEY_F8
#undef KEY_F9
#undef KEY_F10
#undef KEY_F11
#undef KEY_F12

/* ---- 2. BEHAVIOUR ------------------------------------------------------ */

#include "ps2.h"

#include <stdint.h>

/* The acceptance criterion names these by hand, so they are asserted by
 * hand: the arrows Doom moves with, and the key it fires with. */
static void test_movement_and_fire(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_UP),    DOOM_KEY_UPARROW);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_DOWN),  DOOM_KEY_DOWNARROW);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_LEFT),  DOOM_KEY_LEFTARROW);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_RIGHT), DOOM_KEY_RIGHTARROW);

    /*
     * Fire and use are delivered as Doom's ACTION codes, not as the
     * physical keys, because src/doom/m_controls.c binds key_fire to
     * KEY_FIRE and key_use to KEY_USE -- codes no physical key produces.
     * Send KEY_RCTRL or ' ' instead and `gamekeydown[key_fire]` is never
     * set: menus and movement still work and the trigger silently does
     * nothing, which is precisely how this shipped broken the first time.
     */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_CTRL),  DOOM_KEY_FIRE);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SPACE), DOOM_KEY_USE);

    /* Same reasoning for strafe-left/right (m_controls.c binds them to
     * KEY_STRAFE_L/_R), which vanilla also puts on comma and period. */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_COMMA),  DOOM_KEY_STRAFE_L);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_PERIOD), DOOM_KEY_STRAFE_R);

    /* ALT and SHIFT are the exception: key_strafe = KEY_RALT and
     * key_speed = KEY_RSHIFT really are physical-key codes, which is why
     * those two worked even while fire did not. */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_ALT),         DOOM_KEY_RALT);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SHIFT_LEFT),  DOOM_KEY_RSHIFT);
}

static void test_menu_keys(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_ESC),       DOOM_KEY_ESCAPE);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_ENTER),     DOOM_KEY_ENTER);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_TAB),       DOOM_KEY_TAB);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_BACKSPACE), DOOM_KEY_BACKSPACE);

    /* y and n answer Doom's quit and end-game prompts; they must arrive as
     * lowercase characters or M_Responder never matches them. */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_Y), 'y');
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_N), 'n');
}

static void test_function_keys(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_F1),  DOOM_KEY_F1);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_F6),  DOOM_KEY_F6);  /* quicksave */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_F9),  DOOM_KEY_F9);  /* quickload */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_F10), DOOM_KEY_F10); /* quit */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_F12), DOOM_KEY_F12);
}

static void test_modifiers_fold_to_right_hand_keycodes(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_ALT), DOOM_KEY_RALT);

    /* Both PS/2 shifts, one Doom keycode. */
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SHIFT_LEFT),  DOOM_KEY_RSHIFT);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SHIFT_RIGHT), DOOM_KEY_RSHIFT);
}

/*
 * Every letter lowercase, every digit its own character. The cheat matcher
 * (st_stuff.c) compares against lowercase sequences and the weapon keys are
 * '1'..'7', so one wrong case here is a silently broken cheat or an unusable
 * weapon slot. Swept rather than spot-checked so a gap in the middle of the
 * alphabet cannot hide behind its neighbours.
 */
static void test_letters_and_digits(void)
{
    uint8_t i;
    int     ok = 1;

    for (i = 0; i < 26; i++) {
        if (doom_keymap_translate((uint8_t)(KEY_A + i)) !=
            (unsigned char)('a' + i)) {
            ok = 0;
        }
    }
    CU_ASSERT_TRUE(ok);

    ok = 1;
    for (i = 0; i < 10; i++) {
        if (doom_keymap_translate((uint8_t)(KEY_0 + i)) !=
            (unsigned char)('0' + i)) {
            ok = 0;
        }
    }
    CU_ASSERT_TRUE(ok);
}

static void test_punctuation(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_MINUS),     DOOM_KEY_MINUS);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_EQUALS),    DOOM_KEY_EQUALS);
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SLASH),     '/');
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_SEMICOLON), ';');

    /* Comma and period are strafe, not characters -- asserted in
     * test_movement_and_fire() alongside the other action codes. */
}

/*
 * The unmapped cases. KEY_UNKNOWN is the decoder's own "I did not recognise
 * that scancode", and anything past the end of the enum is a key ps2.h has
 * not grown yet. Both must come back DOOM_KEY_NONE rather than passing a
 * value through: ps2_key_t's ordinals are small integers that land squarely
 * inside Doom's ASCII range, so a pass-through turns an unmapped key into a
 * plausible letter.
 */
static void test_unmapped_keys_report_none(void)
{
    CU_ASSERT_EQUAL(doom_keymap_translate(KEY_UNKNOWN), DOOM_KEY_NONE);
    CU_ASSERT_EQUAL(doom_keymap_translate(200),         DOOM_KEY_NONE);
    CU_ASSERT_EQUAL(doom_keymap_translate(255),         DOOM_KEY_NONE);
}

/*
 * Sweep the whole enum: every key from KEY_A to the last one must map to
 * something, since the enum contains nothing Doom has no use for. A table
 * that quietly returned DOOM_KEY_NONE for a run of keys would pass every
 * spot-check above and fail here.
 */
static void test_every_enum_key_is_mapped(void)
{
    uint8_t k;
    int     unmapped = 0;

    for (k = KEY_A; k <= KEY_SEMICOLON; k++) {
        if (doom_keymap_translate(k) == DOOM_KEY_NONE) {
            unmapped++;
        }
    }

    CU_ASSERT_EQUAL(unmapped, 0);
}

void suite_doom_keymap_tests(CU_pSuite s)
{
    CU_add_test(s, "movement arrows and fire map to Doom keycodes",
                test_movement_and_fire);
    CU_add_test(s, "menu keys map to Doom keycodes", test_menu_keys);
    CU_add_test(s, "function keys map to Doom keycodes", test_function_keys);
    CU_add_test(s, "modifier keys fold to right-hand keycodes",
                test_modifiers_fold_to_right_hand_keycodes);
    CU_add_test(s, "letters lowercase and digits pass through",
                test_letters_and_digits);
    CU_add_test(s, "punctuation maps to Doom keycodes", test_punctuation);
    CU_add_test(s, "unmapped keys report DOOM_KEY_NONE",
                test_unmapped_keys_report_none);
    CU_add_test(s, "every ps2_key_t enum key is mapped",
                test_every_enum_key_is_mapped);
}
