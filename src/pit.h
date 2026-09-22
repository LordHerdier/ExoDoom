#pragma once
#include <stdint.h>

void pit_init(uint32_t hz);
uint32_t kernel_get_ticks_ms();
uint8_t pit_take_print_pending();

/*
 * SCRUM-181: clear-on-read flag set by irq0_handler() when a ~60 Hz
 * compositor tick is due, mirroring pit_take_print_pending() above. Kept
 * unconditional (not TESTING-gated) since it's just a flag, unlike the
 * framebuffer memcpy it used to trigger directly -- see fb_compositor.c's
 * fb_compositor_service(), which consumes it from syscall-dispatch context
 * instead of from inside this ISR.
 */
uint8_t pit_take_composite_pending();

#ifdef TESTING
/*
 * SCRUM-170: the stack pointer irq0_handler() observed the last time it ran,
 * captured only in TESTING builds. A CPL-3 -> CPL-0 hardware interrupt is
 * supposed to land on TSS.RSP0 (src/tss.h) exactly like the CPL-3 page fault
 * test_tss_k.c already proves for an *exception* -- this is how a live test
 * checks that for a real IRQ0 instead of just reasoning about it. Reset to 0
 * so a stale value from an earlier suite can't be mistaken for a fresh one.
 */
uint64_t pit_irq0_last_rsp(void);
void pit_irq0_reset_last_rsp(void);
#endif

