#include "ps2.h"
#include "kbd_ring.h"
#include "io.h"
#include "serial.h"
#include "pic.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

/* Status register (port 0x64) bits */
#define PS2_STATUS_OBF  0x01   /* output buffer full — a byte is waiting  */
#define PS2_STATUS_AUX  0x20   /* byte came from the auxiliary (mouse) port */

/* Upper bound on bytes consumed per IRQ1.  The controller's buffer holds far
 * fewer than this; the limit only exists so a wedged controller that reports
 * OBF forever cannot spin the interrupt handler indefinitely. */
#define PS2_IRQ_DRAIN_MAX 32u

/* Upper bound on the polled-read spin (see ps2_read_scancode). */
#define PS2_POLL_SPINS 100000u

static bool ps2_shift = false;
static bool ps2_ctrl = false;
static bool ps2_alt = false;
static bool ps2_extended = false;
static bool ps2_break = false;
static uint8_t ps2_skip = 0;      /* bytes of a sequence left to discard */

static kbd_ring_t kbd_queue;
static volatile uint8_t modifier_state = 0;

/* Drops already announced by kbd_service.  Consumer-owned, so the producer
 * never has to write the drop counter back down. */
static uint32_t kbd_reported_drops = 0;

/*
 * ps2_read_scancode — Polled read of one byte from the keyboard data port.
 *
 * Only for use before IRQ1 is wired up.  The spin is bounded so a keyboard
 * that never produces a byte cannot hang the caller; on timeout the data port
 * is read anyway and returns whatever the controller last latched.  The IRQ
 * path does not use this — see ps2_irq1_handler.
 */
uint8_t ps2_read_scancode(void) {
    for (uint32_t spins = 0; spins < PS2_POLL_SPINS; spins++) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OBF)
            break;
    }
    return inb(PS2_DATA_PORT);
}

void ps2_print_scancode(uint8_t scancode) {
    char buffer[32];
    const char hex_digits[] = "0123456789ABCDEF";

    buffer[0] = 'P';
    buffer[1] = 'S';
    buffer[2] = '/';
    buffer[3] = '2';
    buffer[4] = ' ';
    buffer[5] = 'S';
    buffer[6] = 'c';
    buffer[7] = 'a';
    buffer[8] = 'n';
    buffer[9] = 'c';
    buffer[10] = 'o';
    buffer[11] = 'd';
    buffer[12] = 'e';
    buffer[13] = ':';
    buffer[14] = ' ';
    buffer[15] = '0';
    buffer[16] = 'x';
    buffer[17] = hex_digits[(scancode >> 4) & 0xF];
    buffer[18] = hex_digits[scancode & 0xF];
    buffer[19] = '\n';
    buffer[20] = '\0';

    serial_print(buffer);
}

static ps2_key_t ps2_translate_scancode(uint8_t scancode) {
    if (ps2_extended) {
        switch (scancode) {
            case 0x48: return KEY_UP;
            case 0x50: return KEY_DOWN;
            case 0x4B: return KEY_LEFT;
            case 0x4D: return KEY_RIGHT;
            case 0x1D: return KEY_CTRL;
            case 0x38: return KEY_ALT;
            default:   return KEY_UNKNOWN;
        }
    }

    switch (scancode) {
        case 0x1E: return KEY_A;
        case 0x30: return KEY_B;
        case 0x2E: return KEY_C;
        case 0x20: return KEY_D;
        case 0x12: return KEY_E;
        case 0x21: return KEY_F;
        case 0x22: return KEY_G;
        case 0x23: return KEY_H;
        case 0x17: return KEY_I;
        case 0x24: return KEY_J;
        case 0x25: return KEY_K;
        case 0x26: return KEY_L;
        case 0x32: return KEY_M;
        case 0x31: return KEY_N;
        case 0x18: return KEY_O;
        case 0x19: return KEY_P;
        case 0x10: return KEY_Q;
        case 0x13: return KEY_R;
        case 0x1F: return KEY_S;
        case 0x14: return KEY_T;
        case 0x16: return KEY_U;
        case 0x2F: return KEY_V;
        case 0x11: return KEY_W;
        case 0x2D: return KEY_X;
        case 0x15: return KEY_Y;
        case 0x2C: return KEY_Z;
        case 0x2A: return KEY_SHIFT_LEFT;
        case 0x36: return KEY_SHIFT_RIGHT;
        case 0x1D: return KEY_CTRL;
        case 0x38: return KEY_ALT;
        case 0x39: return KEY_SPACE;
        case 0x0E: return KEY_BACKSPACE;
        case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER;
        case 0x01: return KEY_ESC;
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        default:    return KEY_UNKNOWN;
    }
}

static const char* ps2_key_name(ps2_key_t key) {
    switch (key) {
        case KEY_A: return "KEY_A";
        case KEY_B: return "KEY_B";
        case KEY_C: return "KEY_C";
        case KEY_D: return "KEY_D";
        case KEY_E: return "KEY_E";
        case KEY_F: return "KEY_F";
        case KEY_G: return "KEY_G";
        case KEY_H: return "KEY_H";
        case KEY_I: return "KEY_I";
        case KEY_J: return "KEY_J";
        case KEY_K: return "KEY_K";
        case KEY_L: return "KEY_L";
        case KEY_M: return "KEY_M";
        case KEY_N: return "KEY_N";
        case KEY_O: return "KEY_O";
        case KEY_P: return "KEY_P";
        case KEY_Q: return "KEY_Q";
        case KEY_R: return "KEY_R";
        case KEY_S: return "KEY_S";
        case KEY_T: return "KEY_T";
        case KEY_U: return "KEY_U";
        case KEY_V: return "KEY_V";
        case KEY_W: return "KEY_W";
        case KEY_X: return "KEY_X";
        case KEY_Y: return "KEY_Y";
        case KEY_Z: return "KEY_Z";
        case KEY_SHIFT_LEFT:  return "KEY_SHIFT_LEFT";
        case KEY_SHIFT_RIGHT: return "KEY_SHIFT_RIGHT";
        case KEY_CTRL:        return "KEY_CTRL";
        case KEY_ALT:         return "KEY_ALT";
        case KEY_UP:          return "KEY_UP";
        case KEY_DOWN:        return "KEY_DOWN";
        case KEY_LEFT:        return "KEY_LEFT";
        case KEY_RIGHT:       return "KEY_RIGHT";
        case KEY_ENTER:       return "KEY_ENTER";
        case KEY_SPACE:       return "KEY_SPACE";
        case KEY_BACKSPACE:   return "KEY_BACKSPACE";
        case KEY_TAB:         return "KEY_TAB";
        case KEY_ESC:         return "KEY_ESC";
        case KEY_F1:          return "KEY_F1";
        case KEY_F2:          return "KEY_F2";
        case KEY_F3:          return "KEY_F3";
        case KEY_F4:          return "KEY_F4";
        case KEY_F5:          return "KEY_F5";
        case KEY_F6:          return "KEY_F6";
        case KEY_F7:          return "KEY_F7";
        case KEY_F8:          return "KEY_F8";
        case KEY_F9:          return "KEY_F9";
        case KEY_F10:         return "KEY_F10";
        case KEY_F11:         return "KEY_F11";
        case KEY_F12:         return "KEY_F12";
        default:              return "KEY_UNKNOWN";
    }
}

/* Right Ctrl and right Alt arrive as E0-prefixed copies of the left keys, so
 * the extended flag is what separates MOD_RCTRL/MOD_RALT from their left
 * counterparts. */
static void update_modifier_state(ps2_key_t key, bool pressed, bool extended) {
    uint8_t bit;

    switch (key) {
        case KEY_SHIFT_LEFT:  bit = MOD_LSHIFT; break;
        case KEY_SHIFT_RIGHT: bit = MOD_RSHIFT; break;
        case KEY_CTRL:        bit = extended ? MOD_RCTRL : MOD_LCTRL; break;
        case KEY_ALT:         bit = extended ? MOD_RALT  : MOD_LALT;  break;
        default:              return;
    }

    modifier_state = pressed ? (uint8_t)(modifier_state | bit)
                             : (uint8_t)(modifier_state & (uint8_t)~bit);
}

void kbd_init(void) {
    kbd_reset();
    outb(0x21, inb(0x21) & ~(1 << 1));
}

void kbd_reset(void) {
    kbd_ring_init(&kbd_queue);

    ps2_shift = false;
    ps2_ctrl = false;
    ps2_alt = false;
    ps2_extended = false;
    ps2_break = false;
    ps2_skip = 0;
    modifier_state = 0;
    kbd_reported_drops = 0;
}

void kbd_enqueue(kbd_event_t event) {
    event.modifiers = modifier_state;
    kbd_ring_push(&kbd_queue, event);
}

int kbd_dequeue(kbd_event_t *out) {
    return kbd_ring_pop(&kbd_queue, out);
}

int exo_kbd_poll(kbd_event_t *event_out) {
    if (!event_out) return 0;
    return kbd_dequeue(event_out);
}

uint16_t kbd_pending(void) {
    return kbd_ring_count(&kbd_queue);
}

uint32_t kbd_dropped_count(void) {
    return kbd_ring_dropped(&kbd_queue);
}

uint8_t ps2_get_modifier_state(void) {
    return modifier_state;
}

/*
 * ps2_process_scancode — Decode one byte of the scancode stream.
 *
 * Runs in interrupt context, so it does no serial I/O: at 38400 baud a single
 * logged keystroke costs milliseconds, long enough for the next scancodes to
 * pile up behind it.  Logging happens in kbd_service on the consumer side.
 */
void ps2_process_scancode(uint8_t scancode) {
    /* Pause/Break sends E1 1D 45 E1 9D C5 and has no meaningful key event.
     * Swallow the two bytes after each E1 so 1D is not decoded as Ctrl. */
    if (ps2_skip) {
        ps2_skip--;
        return;
    }

    if (scancode == 0xE1) {
        ps2_skip = 2;
        return;
    }

    if (scancode == 0xE0) {
        ps2_extended = true;
        return;
    }

    if (scancode == 0xF0) {
        /* Scancode set 2 break prefix; the next byte is a release. */
        ps2_break = true;
        return;
    }

    /* Set 1 marks a release with bit 7, on extended keys too: the up arrow
     * makes E0 48 and breaks E0 C8.  Masking the bit off unconditionally is
     * what lets release events reach the queue for extended keys. */
    bool release = (scancode & 0x80) != 0;
    uint8_t code = (uint8_t)(scancode & 0x7F);

    if (ps2_break) {
        release = true;
        ps2_break = false;
    }

    ps2_key_t key = ps2_translate_scancode(code);
    bool extended = ps2_extended;
    ps2_extended = false;

    if (key == KEY_UNKNOWN)
        return;

    bool pressed = !release;

    update_modifier_state(key, pressed, extended);

    if (key == KEY_SHIFT_LEFT || key == KEY_SHIFT_RIGHT) {
        ps2_shift = pressed;
    } else if (key == KEY_CTRL) {
        ps2_ctrl = pressed;
    } else if (key == KEY_ALT) {
        ps2_alt = pressed;
    }

    kbd_event_t ev = {
        .pressed   = pressed ? 1 : 0,
        .key       = (uint8_t)key,
        .modifiers = modifier_state,
    };
    kbd_enqueue(ev);
}

/*
 * ps2_irq1_handler — IRQ1 entry point.
 *
 * Drains every byte the controller has buffered rather than reading one.
 * IRQ1 is edge triggered: bytes left behind are not re-announced, so under
 * rapid typing a one-byte-per-interrupt handler falls permanently behind and
 * loses keys.
 */
void ps2_irq1_handler(void) {
    for (unsigned i = 0; i < PS2_IRQ_DRAIN_MAX; i++) {
        uint8_t status = inb(PS2_STATUS_PORT);

        if (!(status & PS2_STATUS_OBF))
            break;

        uint8_t data = inb(PS2_DATA_PORT);

        /* Keyboard and mouse share port 0x60; a byte tagged AUX belongs to
         * IRQ12 and must not be fed to the scancode decoder. */
        if (status & PS2_STATUS_AUX)
            continue;

        ps2_process_scancode(data);
    }

    pic_send_EOI(1);
}

void irq1_handler(void) {
    ps2_irq1_handler();
}

/*
 * kbd_service — Consumer-side drain.
 *
 * Prints every queued event and reports overflow drops.  Runs in ordinary
 * kernel context where blocking on the serial port is harmless.
 */
void kbd_service(void) {
    kbd_event_t ev;

    while (kbd_dequeue(&ev)) {
        serial_print(ps2_key_name((ps2_key_t)ev.key));
        serial_print(ev.pressed ? " DOWN" : " UP");
        serial_print(" (shift=");
        serial_print((ev.modifiers & (MOD_LSHIFT | MOD_RSHIFT)) ? "1" : "0");
        serial_print(" ctrl=");
        serial_print((ev.modifiers & (MOD_LCTRL | MOD_RCTRL)) ? "1" : "0");
        serial_print(" alt=");
        serial_print((ev.modifiers & (MOD_LALT | MOD_RALT)) ? "1" : "0");
        serial_print(")\n");
    }

    uint32_t dropped = kbd_dropped_count();

    if (dropped != kbd_reported_drops) {
        serial_print("kbd: queue full, dropped ");
        serial_print_u32(dropped - kbd_reported_drops);
        serial_print(" event(s)\n");
        kbd_reported_drops = dropped;
    }
}

bool ps2_shift_active(void) { return ps2_shift; }
bool ps2_ctrl_active(void)  { return ps2_ctrl; }
bool ps2_alt_active(void)   { return ps2_alt; }
