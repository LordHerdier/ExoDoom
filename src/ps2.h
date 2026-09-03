#pragma once
#include <stdint.h>
#include <stdbool.h>

/*
 * ps2.h — PS/2 keyboard driver for IRQ1 handler.
 *
 * Reads scancodes from port 0x60, turns them into press/release key events,
 * and queues them in the event ring (see kbd_ring.h).  The IRQ path does no
 * serial I/O; call kbd_service() from ordinary kernel context to drain and
 * report queued events.
 */

/* Read a single scancode from the PS/2 keyboard port.
 *
 * Polled helper for use before IRQ1 is wired up.  Spins for a bounded number
 * of iterations waiting for the output buffer to fill, then reads port 0x60
 * regardless; it is not used by the interrupt path. */
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

/* Modifier bits reported in kbd_event_t.modifiers */
#define MOD_LSHIFT (1 << 0)
#define MOD_RSHIFT (1 << 1)
#define MOD_LCTRL  (1 << 2)
#define MOD_RCTRL  (1 << 3)
#define MOD_LALT   (1 << 4)
#define MOD_RALT   (1 << 5)

typedef struct {
    uint8_t pressed;           // 1=key down, 0=key up
    uint8_t key;               // ps2_key_t
    uint8_t modifiers;         // MOD_* mask, sampled when the event was queued
} kbd_event_t;

/* IRQ1 handler — drains the controller output buffer and enqueues events */
void ps2_irq1_handler(void);

/* IRQ1 handler symbol used by IRQ stub */
void irq1_handler(void);

/* Decode one scancode byte and queue the resulting event, if any.
 * Called from the IRQ path; exposed so tests can drive the decoder without
 * hardware. */
void ps2_process_scancode(uint8_t scancode);

/* Driver init + APIs */
void kbd_init(void);

/* Queue an event verbatim, modifier field included.  Single-producer: safe
 * from the IRQ1 path, and from elsewhere only with IRQ1 unable to run. */
void kbd_enqueue(kbd_event_t event);
int kbd_dequeue(kbd_event_t *out);
int exo_kbd_poll(kbd_event_t *event_out);
uint8_t ps2_get_modifier_state(void);

/* Empty the event queue and clear decoder/modifier state.  Not safe against a
 * concurrent IRQ1 — call with IRQ1 masked or before interrupts are enabled. */
void kbd_reset(void);

/* Events currently queued, and events discarded because the queue was full. */
uint16_t kbd_pending(void);
uint32_t kbd_dropped_count(void);

/* Bytes the IRQ path discarded as AUX (mouse) traffic.  Non-zero on a
 * controller where status bit 5 is not an AUX flag means lost keystrokes. */
uint32_t kbd_aux_count(void);

/* Consumer side: drain queued events to serial and report any overflow.
 * Call from ordinary kernel context, never from an interrupt handler. */
void kbd_service(void);

/* Access modifier state.  Derived from the per-key modifier mask, so holding
 * one key of a pair while releasing the other still reads as active. */
bool ps2_shift_active(void);
bool ps2_ctrl_active(void);
bool ps2_alt_active(void);
