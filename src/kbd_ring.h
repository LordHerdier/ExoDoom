#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ps2.h"

/*
 * kbd_ring.h — Lock-free keyboard event ring buffer (SCRUM-18).
 *
 * Decouples the IRQ1 handler, which must never block, from the consumer
 * (kernel loop today, exo_kbd_poll syscall in SCRUM-39).  Both key press and
 * key release events are queued, so a consumer that polls slowly still sees
 * every transition in the order the hardware reported it.
 *
 * Concurrency model: strictly single-producer / single-consumer.  The producer
 * is the IRQ1 handler (kbd_ring_push only); the consumer is ordinary kernel
 * code (kbd_ring_pop only).  On x86 this needs no locking — stores are not
 * reordered with other stores, and loads are not reordered with other loads —
 * provided the slot is fully written before head advances and the compiler is
 * stopped from reordering the two.  kbd_ring.c inserts those barriers.
 *
 * Calling kbd_ring_push from non-interrupt context, or popping from more than
 * one context, breaks that assumption and is not supported.
 */

/* Capacity in events.  Must be a power of two so the index wrap is a mask.
 * A keystroke costs two events (down + up), so 256 absorbs 128 keystrokes
 * between polls — far beyond any burst a human can type into one frame. */
#define KBD_RING_CAPACITY 256u
#define KBD_RING_MASK     (KBD_RING_CAPACITY - 1u)

typedef struct {
    kbd_event_t     slots[KBD_RING_CAPACITY];
    volatile uint16_t head;     /* next write slot — producer owns  */
    volatile uint16_t tail;     /* next read slot  — consumer owns  */
    volatile uint32_t dropped;  /* events lost to overflow — producer owns */
} kbd_ring_t;

/* Reset to empty and clear the drop counter.  Call before interrupts are
 * enabled; it is not safe against a concurrent producer. */
void kbd_ring_init(kbd_ring_t *ring);

/* Producer side.  Returns 1 if the event was stored, 0 if the ring was full,
 * in which case the event is discarded and the drop counter is incremented.
 * Never blocks and never corrupts queued events. */
int kbd_ring_push(kbd_ring_t *ring, kbd_event_t event);

/* Consumer side.  Returns 1 and fills *out with the oldest event, or 0 if the
 * ring is empty.  *out is untouched when the ring is empty. */
int kbd_ring_pop(kbd_ring_t *ring, kbd_event_t *out);

/* Number of events currently queued (0 .. KBD_RING_CAPACITY - 1). */
uint16_t kbd_ring_count(const kbd_ring_t *ring);

bool kbd_ring_is_empty(const kbd_ring_t *ring);
bool kbd_ring_is_full(const kbd_ring_t *ring);

/* Total events discarded because the ring was full since the last init.
 * Saturates at UINT32_MAX rather than wrapping. */
uint32_t kbd_ring_dropped(const kbd_ring_t *ring);
