#include "kbd_ring.h"

/*
 * kbd_ring.c — Single-producer / single-consumer keyboard event ring.
 *
 * One slot is always left empty so that head == tail means "empty" and
 * head + 1 == tail means "full" without a separate count field that both
 * sides would have to write.  Usable depth is therefore
 * KBD_RING_CAPACITY - 1 events.
 */

/* Stop the compiler from moving memory accesses across this point.  x86
 * hardware does not reorder store-store or load-load, so this is the only
 * barrier the SPSC handoff needs. */
static inline void kbd_ring_barrier(void)
{
    __asm__ volatile ("" ::: "memory");
}

void kbd_ring_init(kbd_ring_t *ring)
{
    if (!ring) return;

    ring->head    = 0;
    ring->tail    = 0;
    ring->dropped = 0;
}

int kbd_ring_push(kbd_ring_t *ring, kbd_event_t event)
{
    if (!ring) return 0;

    uint16_t head      = ring->head;
    uint16_t next_head = (uint16_t)((head + 1u) & KBD_RING_MASK);

    if (next_head == ring->tail) {
        /* Full: drop the newest event rather than overwrite queued history,
         * so the consumer still sees an intact prefix of the input stream.
         * Saturate the counter so a long overflow cannot wrap it to zero. */
        if (ring->dropped != UINT32_MAX)
            ring->dropped++;
        return 0;
    }

    ring->slots[head] = event;

    /* The slot must be fully written before the consumer can observe the
     * advanced head, or it could read a half-populated event. */
    kbd_ring_barrier();

    ring->head = next_head;
    return 1;
}

int kbd_ring_pop(kbd_ring_t *ring, kbd_event_t *out)
{
    if (!ring || !out) return 0;

    uint16_t tail = ring->tail;

    if (ring->head == tail)
        return 0;                       /* empty */

    *out = ring->slots[tail];

    /* Copy out before releasing the slot back to the producer. */
    kbd_ring_barrier();

    ring->tail = (uint16_t)((tail + 1u) & KBD_RING_MASK);
    return 1;
}

uint16_t kbd_ring_count(const kbd_ring_t *ring)
{
    if (!ring) return 0;
    return (uint16_t)((ring->head - ring->tail) & KBD_RING_MASK);
}

bool kbd_ring_is_empty(const kbd_ring_t *ring)
{
    return !ring || ring->head == ring->tail;
}

bool kbd_ring_is_full(const kbd_ring_t *ring)
{
    if (!ring) return false;
    return (uint16_t)((ring->head + 1u) & KBD_RING_MASK) == ring->tail;
}

uint32_t kbd_ring_dropped(const kbd_ring_t *ring)
{
    return ring ? ring->dropped : 0;
}
