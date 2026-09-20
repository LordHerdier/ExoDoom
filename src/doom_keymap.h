#ifndef DOOM_KEYMAP_H
#define DOOM_KEYMAP_H

/*
 * doom_keymap.h — ps2_key_t -> Doom keycode translation (SCRUM-40).
 *
 * The kernel's keyboard syscall hands a LibOS a *decoded* key: exo_kbd_poll
 * (#6) fills an exo_kbd_event_t whose `key` is a ps2_key_t index (KEY_A,
 * KEY_UP, ...), never a raw set-1 scancode -- src/ps2.c's decoder has already
 * eaten the wire bytes, the 0xE0 prefixes and the make/break bit. That was a
 * deliberate split: exo_syscall.h's own comment on #6 says the remaining
 * ps2_key_t -> Doom keycode mapping "stays LibOS policy ... in the Doom
 * port". This is that mapping, and it is the last piece SCRUM-79's DG_GetKey
 * needs.
 *
 * It is a pure function over one key and the modifier mask: no state, no
 * hardware, no syscalls. That is what lets it be unit-tested exhaustively in
 * ring 0 (tests/kernel/test_doom_keymap_k.c) while shipping inside the ring-3
 * Doom LibOS.
 *
 * ── Why Doom's keycodes are restated here ────────────────────────────
 *
 * src/doom/doomkeys.h and src/ps2.h CANNOT BE INCLUDED IN THE SAME
 * TRANSLATION UNIT. Nineteen names collide outright -- KEY_ENTER, KEY_TAB,
 * KEY_BACKSPACE, KEY_MINUS, KEY_EQUALS and KEY_F1..KEY_F12 -- as an enum
 * constant on one side and an object-like macro on the other. Include
 * doomkeys.h first and the preprocessor rewrites ps2.h's enum body
 * (`KEY_ENTER,` becomes `13,`) into a syntax error; include ps2.h first and
 * every later textual use of those names silently means Doom's value instead
 * of the PS/2 one, which compiles and is wrong.
 *
 * So the mapping table cannot name both sides symbolically, and one of them
 * has to be restated. Doom's side is the safe one to restate: these values
 * are fixed by the game's own event protocol and by every WAD's default
 * bindings, whereas ps2_key_t is an enum whose ordinals shift the moment
 * someone inserts a key in the middle of it. Restating the volatile side
 * would be a silent-breakage machine.
 *
 * The restatement is CHECKED, not merely commented, in the same spirit as
 * src/doom_panic.c's serial cap: tests/kernel/test_doom_keymap_k.c includes
 * doomkeys.h in a preamble whose only job is to _Static_assert every constant
 * below against the real one, then #undefs the colliding macros before it
 * includes ps2.h. Change a value here without changing it there and the build
 * fails naming both.
 */

#include <stdint.h>

/* Doom keycodes, restated from src/doom/doomkeys.h -- see the header comment
 * above for why this file cannot just include it. Names are DOOM_KEY_* rather
 * than KEY_* precisely so that they collide with neither side. */
#define DOOM_KEY_RIGHTARROW 0xae
#define DOOM_KEY_LEFTARROW  0xac
#define DOOM_KEY_UPARROW    0xad
#define DOOM_KEY_DOWNARROW  0xaf
#define DOOM_KEY_STRAFE_L   0xa0
#define DOOM_KEY_STRAFE_R   0xa1
#define DOOM_KEY_USE        0xa2
#define DOOM_KEY_FIRE       0xa3
#define DOOM_KEY_ESCAPE     27
#define DOOM_KEY_ENTER      13
#define DOOM_KEY_TAB        9
#define DOOM_KEY_BACKSPACE  0x7f
#define DOOM_KEY_PAUSE      0xff
#define DOOM_KEY_EQUALS     0x3d
#define DOOM_KEY_MINUS      0x2d
#define DOOM_KEY_RSHIFT     (0x80 + 0x36)
#define DOOM_KEY_RCTRL      (0x80 + 0x1d)
#define DOOM_KEY_RALT       (0x80 + 0x38)
#define DOOM_KEY_F1         (0x80 + 0x3b)
#define DOOM_KEY_F2         (0x80 + 0x3c)
#define DOOM_KEY_F3         (0x80 + 0x3d)
#define DOOM_KEY_F4         (0x80 + 0x3e)
#define DOOM_KEY_F5         (0x80 + 0x3f)
#define DOOM_KEY_F6         (0x80 + 0x40)
#define DOOM_KEY_F7         (0x80 + 0x41)
#define DOOM_KEY_F8         (0x80 + 0x42)
#define DOOM_KEY_F9         (0x80 + 0x43)
#define DOOM_KEY_F10        (0x80 + 0x44)
#define DOOM_KEY_F11        (0x80 + 0x57)
#define DOOM_KEY_F12        (0x80 + 0x58)

/*
 * "No Doom key for this one." Doom's own keycode space has no spare value
 * that means nothing -- 0 is KEYP_0 and KEYP_PERIOD -- but those are keypad
 * aliases the translation below never produces, and DG_GetKey's contract is
 * to report "no event" by returning 0 rather than by passing a key through.
 * So 0 is usable here as the sentinel, and doom_keymap_translate()'s return
 * value must be tested before the key is handed to the engine.
 */
#define DOOM_KEY_NONE 0

/*
 * Translate one decoded PS/2 key into the Doom keycode the engine expects.
 *
 * `key` is a ps2_key_t, taken as uint8_t because that is how it crosses the
 * syscall boundary (exo_kbd_event_t.key) and because this header must not
 * include src/ps2.h -- a caller that includes both this and doomkeys.h would
 * otherwise inherit the collision described above.
 *
 * Deliberately NOT taking the modifier mask, despite exo_kbd_poll delivering
 * one. It would have nothing to decide. The obvious use would be picking a
 * side for the keys ps2_key_t merges and Doom splits, but Doom does not
 * actually split them: doomkeys.h defines KEY_LALT as KEY_RALT, and it has a
 * single KEY_RSHIFT and a single KEY_RCTRL with no left-hand counterpart at
 * all. The other use would be producing shifted characters, and that is not
 * this layer's job either -- Doom carries shift as its own key event and
 * upshifts in M_Responder where it wants to (typing a savegame name).
 *
 * So the mask is real information with no consumer here, and a parameter
 * that is always ignored is worse than one that does not exist. Add it back
 * the day something needs it.
 *
 * Returns DOOM_KEY_NONE for any key Doom has no use for, and for
 * KEY_UNKNOWN. The caller must drop those rather than forwarding them.
 */
unsigned char doom_keymap_translate(uint8_t key);

#endif /* DOOM_KEYMAP_H */
