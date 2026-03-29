#include "pit.h"
#include "io.h"
#include "serial.h"
#include "pic.h"

static volatile uint32_t ticks = 0;
static uint32_t frequency = 1000;
static volatile uint8_t print_pending = 0;
static uint32_t divisor = 1193;

void pit_init(uint32_t hz) {
    frequency = hz;
    uint32_t divisor = (1193180 + hz / 2) / hz;

    outb(0x43, 0x36);

    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

uint32_t timer_ticks() {
    return ticks;
}

uint32_t kernel_get_ticks_ms() {
    return (ticks * divisor * 1000) / 1193180;
}

void irq0_handler() {
    ticks++;
    
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
