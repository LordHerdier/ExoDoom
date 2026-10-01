#include "pit.h"
#include "io.h"
#include "pic.h"
#include "speaker.h"

static volatile uint32_t ticks = 0;
static uint32_t frequency = 1000;
static volatile uint8_t print_pending = 0;
static volatile uint8_t composite_pending = 0;

void pit_init(uint32_t hz) {
    frequency = hz;
    uint32_t divisor = (1193180 + hz / 2) / hz;

    outb(0x43, 0x36);

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint32_t kernel_get_ticks_ms() {
    return (uint64_t)ticks * 1000 / frequency;
}

#ifdef TESTING
static volatile uint64_t irq0_last_rsp = 0;

uint64_t pit_irq0_last_rsp(void) {
    return irq0_last_rsp;
}

void pit_irq0_reset_last_rsp(void) {
    irq0_last_rsp = 0;
}
#endif

void irq0_handler() {
    ticks++;

    /* SCRUM-98: end a timed PC speaker tone once its duration is up. This is
     * what makes speaker_tone() -- and so exo_sound_tone -- non-blocking. */
    speaker_tick(kernel_get_ticks_ms());

#ifdef TESTING
    /* This function's own stack frame sits wherever the CPU switched RSP to
     * on entry -- TSS.RSP0's stack if the interrupt was taken at CPL 3,
     * unchanged if it was already at CPL 0. __builtin_frame_address(0) reads
     * it without any hand-written asm offset math into the interrupt frame
     * isr.s's irq0_stub built. */
    irq0_last_rsp = (uint64_t)(uintptr_t)__builtin_frame_address(0);
#endif

    if (ticks % frequency == 0) {
        print_pending = 1;
    }

    /* SCRUM-112 / SCRUM-181: the running context's virtual framebuffer needs
     * compositing onto the real one at ~60 Hz rather than every tick -- it's
     * a several-hundred-KB to multi-MB memcpy (src/fb_compositor.c), and
     * nothing needs it more often than the eye can tell. That memcpy used to
     * run right here, inside irq0_handler(), which held IF hardware-cleared
     * for the whole copy -- a much longer interrupts-off window than
     * anything else in the kernel does synchronously, risking delayed IRQ1
     * (keyboard) servicing and PIT jitter. Setting a flag here instead and
     * letting fb_compositor_service() (src/fb_compositor.c) consume it from
     * syscall-dispatch context -- IF stays off for that copy too (see that
     * function's own comment for why re-enabling it would race
     * context_switch_request()), but it's the syscall's IF=0 window rather
     * than the timer ISR's -- gets the memcpy out from under this ISR
     * entirely. Just a flag
     * write, so unlike the old direct call this needs no #ifndef TESTING
     * guard: the framebuffer/fb_binding/fb_shadow init state that guard
     * protected against only matters to the consumer, not to setting a
     * uint8_t. */
    uint32_t composite_period = (frequency >= 60) ? frequency / 60 : 1;
    if (ticks % composite_period == 0) {
        composite_pending = 1;
    }

    pic_send_EOI(0);
}

uint8_t pit_take_print_pending() {
    if (!print_pending) return 0;

    print_pending = 0;
    return 1;
}

uint8_t pit_take_composite_pending() {
    if (!composite_pending) return 0;

    composite_pending = 0;
    return 1;
}
