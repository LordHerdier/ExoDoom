#include "pit.h"
#include "io.h"
#include "pic.h"

static volatile uint32_t ticks = 0;
static uint32_t frequency = 1000;
static volatile uint8_t print_pending = 0;

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

    pic_send_EOI(0);
}

uint8_t pit_take_print_pending() {
    if (!print_pending) return 0;

    print_pending = 0;
    return 1;
}
