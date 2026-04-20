#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * ps2.h — PS/2 keyboard driver for IRQ1 handler.
 *
 * Reads scancodes from port 0x60 and prints them to serial.
 */

/* Read a single scancode from the PS/2 keyboard port */
uint8_t ps2_read_scancode(void);

/* Format and print a scancode to serial output */
void ps2_print_scancode(uint8_t scancode);

typedef enum {
    KEY_UNKNOWN = 0,
    KEY_A,
    KEY_B,
    KEY_C,
    KEY_D,
    KEY_E,
    KEY_F,
    KEY_G,
    KEY_H,
    KEY_I,
    KEY_J,
    KEY_K,
    KEY_L,
    KEY_M,
    KEY_N,
    KEY_O,
    KEY_P,
    KEY_Q,
    KEY_R,
    KEY_S,
    KEY_T,
    KEY_U,
    KEY_V,
    KEY_W,
    KEY_X,
    KEY_Y,
    KEY_Z,
    KEY_SHIFT_LEFT,
    KEY_SHIFT_RIGHT,
    KEY_CTRL,
    KEY_ALT,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_SPACE,
    KEY_BACKSPACE,
    KEY_TAB,
    KEY_ESC,
    KEY_F1,
    KEY_F2,
    KEY_F3,
    KEY_F4,
    KEY_F5,
    KEY_F6,
    KEY_F7,
    KEY_F8,
    KEY_F9,
    KEY_F10,
    KEY_F11,
    KEY_F12,
} ps2_key_t;

typedef struct {
    uint8_t pressed;           // 1=key down, 0=key up
    uint8_t key;               // ps2_key_t
    uint8_t modifiers;         // MOD_* mask
} kbd_event_t;

/* IRQ1 handler — reads scancode and enqueues it */
void ps2_irq1_handler(void);

/* IRQ1 handler symbol used by IRQ stub */
void irq1_handler(void);

/* Driver init + APIs */
void kbd_init(void);
void kbd_enqueue(kbd_event_t event);
int kbd_dequeue(kbd_event_t *out);
int exo_kbd_poll(kbd_event_t *event_out);
uint8_t ps2_get_modifier_state(void);

/* Access modifier state */
bool ps2_shift_active(void);
bool ps2_ctrl_active(void);
bool ps2_alt_active(void);
