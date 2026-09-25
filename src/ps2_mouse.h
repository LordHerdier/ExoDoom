#pragma once
#include <stdint.h>

/*
 * ps2_mouse.h — PS/2 mouse driver for IRQ12 (SCRUM-52).
 *
 * Mirrors src/ps2.h's keyboard driver shape, but the consumer-facing state is
 * an accumulator rather than a ring: exo_mouse_poll's contract (#7,
 * docs/syscall_spec.md) is "deltas accumulate between polls and reset on
 * read, buttons are a level, not a delta" -- there is nothing to queue.
 */

typedef struct {
    int16_t dx;          /* accumulated X movement since the last poll */
    int16_t dy;          /* accumulated Y movement since the last poll */
    uint8_t buttons;      /* level: bit0 left, bit1 right, bit2 middle  */
} ps2_mouse_state_t;

/* Controller init: enables the second (aux) PS/2 port, enables IRQ12 at the
 * controller, and tells the mouse itself to start streaming packets (the
 * 0xA8 / command-byte / 0xF4 sequence). Call once, before pic_unmask_irq(12)
 * -- mirrors kbd_init()'s placement relative to pic_unmask_irq(1). */
void ps2_mouse_init(void);

/* IRQ12 handler symbol used by the IRQ stub (src/isr.s). */
void irq12_handler(void);

/* IRQ12 entry point: drains the controller output buffer, feeding every
 * AUX-tagged byte to ps2_mouse_process_byte(). Mirrors ps2_irq1_handler's
 * bounded-drain shape (src/ps2.c). */
void ps2_mouse_irq12_handler(void);

/* Decode one raw byte of the 3-byte packet stream and fold it into the
 * accumulator. Exposed so tests can drive the decoder without hardware,
 * same reasoning as ps2_process_scancode(). */
void ps2_mouse_process_byte(uint8_t byte);

/* Consumer side: copy the current accumulator into *out, then reset dx/dy to
 * zero. buttons is a level and is left as-is. Single-producer (IRQ12) /
 * single-consumer (the #7 syscall handler), same concurrency model as
 * kbd_ring. */
void ps2_mouse_poll(ps2_mouse_state_t *out);

/* Reset the accumulator and packet-decode state. Not safe against a
 * concurrent IRQ12 -- call with IRQ12 masked or before interrupts are
 * enabled. */
void ps2_mouse_reset(void);

/* Bytes IRQ12 discarded because the controller did not tag them AUX -- the
 * mirror of kbd_aux_count(). Non-zero means a keyboard byte was consumed by
 * the mouse path instead of IRQ1's. */
uint32_t ps2_mouse_non_aux_count(void);

/* Times ps2_mouse_irq12_handler exhausted its drain bound with OBF still
 * set. Mirrors kbd_irq_overrun_count(). */
uint32_t ps2_mouse_irq_overrun_count(void);

/* Packets rejected because byte 0's alignment-check bit (bit 3) was clear --
 * a desync that the decoder resyncs from on the next byte that does have it
 * set. */
uint32_t ps2_mouse_resync_count(void);
