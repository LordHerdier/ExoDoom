#include "ps2.h"
#include "io.h"
#include "serial.h"
#include "pic.h"

#define PS2_DATA_PORT 0x60
#define PS2_STATUS_PORT 0x64

static bool ps2_shift = false;
static bool ps2_ctrl = false;
static bool ps2_alt = false;
static bool ps2_extended = false;
static bool ps2_break = false;

#define KBD_BUFFER_SIZE 64
static kbd_event_t kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint8_t kbd_head = 0;
static volatile uint8_t kbd_tail = 0;
static volatile uint8_t modifier_state = 0;

#define MOD_LSHIFT (1 << 0)
#define MOD_RSHIFT (1 << 1)
#define MOD_LCTRL  (1 << 2)
#define MOD_RCTRL  (1 << 3)
#define MOD_LALT   (1 << 4)
#define MOD_RALT   (1 << 5)

uint8_t ps2_read_scancode(void) {
    while (!(inb(PS2_STATUS_PORT) & 0x01)) {
        ;
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
        default:              return "KEY_UNKNOWN";
    }
}

static void update_modifier_state(ps2_key_t key, bool pressed) {
    switch (key) {
        case KEY_SHIFT_LEFT:
            modifier_state = pressed ? modifier_state | MOD_LSHIFT : modifier_state & ~MOD_LSHIFT;
            break;
        case KEY_SHIFT_RIGHT:
            modifier_state = pressed ? modifier_state | MOD_RSHIFT : modifier_state & ~MOD_RSHIFT;
            break;
        case KEY_CTRL:
            modifier_state = pressed ? modifier_state | MOD_LCTRL : modifier_state & ~MOD_LCTRL;
            break;
        case KEY_ALT:
            modifier_state = pressed ? modifier_state | MOD_LALT : modifier_state & ~MOD_LALT;
            break;
        default:
            break;
    }
}

void kbd_init(void) {
    outb(0x21, inb(0x21) & ~(1 << 1));
}

void kbd_enqueue(kbd_event_t event) {
    uint8_t next_head = (kbd_head + 1) & (KBD_BUFFER_SIZE - 1);
    if (next_head == kbd_tail) {
        return;
    }

    event.modifiers = modifier_state;
    kbd_buffer[kbd_head] = event;
    kbd_head = next_head;
}

int kbd_dequeue(kbd_event_t *out) {
    if (kbd_head == kbd_tail) {
        return 0;
    }

    *out = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) & (KBD_BUFFER_SIZE - 1);
    return 1;
}

int exo_kbd_poll(kbd_event_t *event_out) {
    if (!event_out) return 0;
    return kbd_dequeue(event_out);
}

uint8_t ps2_get_modifier_state(void) {
    return modifier_state;
}

static void ps2_report_key(ps2_key_t key, bool pressed) {
    if (key == KEY_UNKNOWN) return;

    update_modifier_state(key, pressed);

    if (key == KEY_SHIFT_LEFT || key == KEY_SHIFT_RIGHT) {
        ps2_shift = pressed;
    } else if (key == KEY_CTRL) {
        ps2_ctrl = pressed;
    } else if (key == KEY_ALT) {
        ps2_alt = pressed;
    }

    const char* name = ps2_key_name(key);
    serial_print(name);
    serial_print(pressed ? " DOWN" : " UP");
    serial_print(" (shift=");
    serial_print(ps2_shift ? "1" : "0");
    serial_print(" ctrl=");
    serial_print(ps2_ctrl ? "1" : "0");
    serial_print(" alt=");
    serial_print(ps2_alt ? "1" : "0");
    serial_print(")\n");
}

void ps2_process_scancode(uint8_t scancode) {
    if (scancode == 0xE0) {
        ps2_extended = true;
        return;
    }

    if (scancode == 0xF0) {
        ps2_break = true;
        return;
    }

    bool release = false;
    uint8_t code = scancode;

    if (!ps2_extended && (scancode & 0x80)) {
        release = true;
        code = scancode & 0x7F;
    }

    if (ps2_break) {
        release = true;
        ps2_break = false;
    }

    ps2_print_scancode(scancode);

    ps2_key_t key = ps2_translate_scancode(code);

    if (key != KEY_UNKNOWN) {
        bool pressed = !release;
        ps2_report_key(key, pressed);
        kbd_event_t ev = { .pressed = pressed ? 1 : 0, .key = (uint8_t)key, .modifiers = modifier_state };
        kbd_enqueue(ev);
    }

    ps2_extended = false;
}

void ps2_irq1_handler(void) {
    uint8_t scancode = ps2_read_scancode();
    ps2_process_scancode(scancode);
    pic_send_EOI(1);
}

void irq1_handler(void) {
    ps2_irq1_handler();
}

bool ps2_shift_active(void) { return ps2_shift; }
bool ps2_ctrl_active(void)  { return ps2_ctrl; }
bool ps2_alt_active(void)   { return ps2_alt; }
