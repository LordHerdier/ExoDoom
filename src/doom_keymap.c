/*
 * doom_keymap.c — the ps2_key_t -> Doom keycode table (SCRUM-40).
 *
 * See src/doom_keymap.h for the contract and, in particular, for why this
 * file includes src/ps2.h but never src/doom/doomkeys.h.
 *
 * Lives outside src/doom/ for the same reason src/doom_panic.c and
 * src/doom_net_stub.c do: the vendored tree stays a clean drop-in (SCRUM-63),
 * so anything this port writes itself sits beside it rather than inside it.
 * It is linked into the libos_doom ring-3 target and compiled again, with
 * -DEXO_KERNEL, into the kernel for its own unit tests -- it has no state and
 * makes no syscalls, so both builds are the same code.
 */

#include "doom_keymap.h"
#include "ps2.h"

unsigned char doom_keymap_translate(uint8_t key)
{
    switch ((ps2_key_t)key) {

    /*
     * Movement and action.
     *
     * Doom's four arrow keycodes are what its default bindings use for
     * forward/back/turn, and KEY_STRAFE_L/R, KEY_USE and KEY_FIRE are
     * separate keycodes rather than characters -- the engine compares
     * against them directly in G_BuildTiccmd. Mapping the arrows and
     * leaving the rest to the letter keys below is what makes the
     * out-of-the-box Doom control scheme work here with no config file,
     * which matters because there is no writable one (src/stdio.c's fopen
     * serves registered blobs only).
     */
    case KEY_UP:    return DOOM_KEY_UPARROW;
    case KEY_DOWN:  return DOOM_KEY_DOWNARROW;
    case KEY_LEFT:  return DOOM_KEY_LEFTARROW;
    case KEY_RIGHT: return DOOM_KEY_RIGHTARROW;

    /*
     * CTRL is fire and SPACE is use -- delivered as Doom's ACTION keycodes,
     * not as the physical keys.
     *
     * This is the one place where this port must not follow chocolate
     * doom's convention, and getting it wrong is invisible until you try to
     * shoot. Chocolate doom binds key_fire = KEY_RCTRL and key_use = ' ',
     * so a platform layer there reports the physical key and the binding
     * layer resolves it. This vendored tree does not:
     *
     *     src/doom/m_controls.c:35   int key_fire = KEY_FIRE;
     *     src/doom/m_controls.c:36   int key_use  = KEY_USE;
     *
     * KEY_FIRE (0xa3) and KEY_USE (0xa2) are abstract action codes with no
     * physical key behind them, so nothing ever sets gamekeydown[] for them
     * unless the platform layer sends them itself. G_BuildTiccmd() tests
     * `gamekeydown[key_fire]` (g_game.c:431) and would simply never see a
     * press.
     *
     * That is exactly the bug this mapping shipped with: menus, movement,
     * strafe and run all worked -- because key_strafe = KEY_RALT and
     * key_speed = KEY_RSHIFT, which the modifier cases below already match
     * -- while the trigger did nothing at all.
     */
    case KEY_CTRL:  return DOOM_KEY_FIRE;
    case KEY_SPACE: return DOOM_KEY_USE;

    /*
     * The three modifier keys all collapse to Doom's right-hand keycode,
     * and none of them needs the modifier mask to decide that.
     *
     * ALT: doomkeys.h defines KEY_LALT as KEY_RALT outright -- one keycode,
     * two names. SHIFT: ps2_key_t distinguishes the sides and Doom does not
     * (there is no KEY_LSHIFT), so both fold into KEY_RSHIFT. CTRL is
     * handled above with fire, and is the mirror image: ps2_key_t merges
     * the sides and Doom has only the right-hand code anyway.
     */
    case KEY_ALT:
        return DOOM_KEY_RALT;

    case KEY_SHIFT_LEFT:
    case KEY_SHIFT_RIGHT:
        return DOOM_KEY_RSHIFT;

    /* Menu and text editing. */
    case KEY_ESC:       return DOOM_KEY_ESCAPE;
    case KEY_ENTER:     return DOOM_KEY_ENTER;
    case KEY_TAB:       return DOOM_KEY_TAB;
    case KEY_BACKSPACE: return DOOM_KEY_BACKSPACE;

    /*
     * Function keys: the whole menu shelf (F1 help, F2 save, F3 load,
     * F4 volume, F5 detail, F6 quicksave, F7 end game, F8 messages,
     * F9 quickload, F10 quit, F11 gamma, F12 spy).
     */
    case KEY_F1:  return DOOM_KEY_F1;
    case KEY_F2:  return DOOM_KEY_F2;
    case KEY_F3:  return DOOM_KEY_F3;
    case KEY_F4:  return DOOM_KEY_F4;
    case KEY_F5:  return DOOM_KEY_F5;
    case KEY_F6:  return DOOM_KEY_F6;
    case KEY_F7:  return DOOM_KEY_F7;
    case KEY_F8:  return DOOM_KEY_F8;
    case KEY_F9:  return DOOM_KEY_F9;
    case KEY_F10: return DOOM_KEY_F10;
    case KEY_F11: return DOOM_KEY_F11;
    case KEY_F12: return DOOM_KEY_F12;

    /*
     * Letters and digits go through as their lowercase ASCII character.
     *
     * That is what Doom expects: its menu code compares event->data1
     * against plain chars ('y' to confirm a quit, 'n' to decline), the
     * cheat-code matcher (st_stuff.c) accumulates characters, and the
     * automap binds 'm'/'f'/'g'. Lowercase specifically, because
     * M_StringToUpper is applied by the code that wants uppercase --
     * feeding it uppercase already would break the cheat matcher, which
     * compares against lowercase sequences.
     *
     * Shift is deliberately not applied. Doom's own event model carries
     * the shift state as a separate key event (KEY_RSHIFT above), and the
     * one place it needs a shifted character -- typing a savegame name --
     * upshifts it itself in M_Responder.
     */
    case KEY_A: return 'a';
    case KEY_B: return 'b';
    case KEY_C: return 'c';
    case KEY_D: return 'd';
    case KEY_E: return 'e';
    case KEY_F: return 'f';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_I: return 'i';
    case KEY_J: return 'j';
    case KEY_K: return 'k';
    case KEY_L: return 'l';
    case KEY_M: return 'm';
    case KEY_N: return 'n';
    case KEY_O: return 'o';
    case KEY_P: return 'p';
    case KEY_Q: return 'q';
    case KEY_R: return 'r';
    case KEY_S: return 's';
    case KEY_T: return 't';
    case KEY_U: return 'u';
    case KEY_V: return 'v';
    case KEY_W: return 'w';
    case KEY_X: return 'x';
    case KEY_Y: return 'y';
    case KEY_Z: return 'z';

    /* Digits double as the weapon-select keys (1..7 in Doom II). */
    case KEY_0: return '0';
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';

    /*
     * Punctuation. MINUS and EQUALS have dedicated Doom keycodes because
     * they resize the view; the rest are plain characters, and SLASH is
     * worth having for the chat prompt.
     */
    case KEY_MINUS:     return DOOM_KEY_MINUS;
    case KEY_EQUALS:    return DOOM_KEY_EQUALS;

    /*
     * Comma and period strafe, for the same reason CTRL fires: this tree
     * binds key_strafeleft/key_straferight to KEY_STRAFE_L/KEY_STRAFE_R
     * (m_controls.c:33-34), which are action codes no physical key
     * produces. Vanilla's own defaults put strafe on these two keys, so the
     * muscle memory matches.
     *
     * The cost is that ',' and '.' can no longer be typed as characters --
     * which today only affects naming a savegame, and saving cannot work at
     * all until there is a writable filesystem (the FILE* shim in
     * src/stdio.c refuses every write mode). ALT+arrow still strafes
     * regardless, since key_strafe = KEY_RALT.
     */
    case KEY_COMMA:     return DOOM_KEY_STRAFE_L;
    case KEY_PERIOD:    return DOOM_KEY_STRAFE_R;

    case KEY_SLASH:     return '/';
    case KEY_SEMICOLON: return ';';

    /*
     * KEY_UNKNOWN and anything ps2_key_t grows in future. Returning
     * DOOM_KEY_NONE rather than passing the raw value through is the
     * difference between "this key does nothing" and "this key does
     * whatever Doom keycode happens to share that number" -- ps2_key_t's
     * small ordinals land squarely on Doom's ASCII range, so a
     * pass-through would turn an unmapped key into a plausible letter.
     */
    default:
        return DOOM_KEY_NONE;
    }
}
