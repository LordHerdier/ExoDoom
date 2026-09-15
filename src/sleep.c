#include <stdint.h>
#include "pit.h"
#include "sleep.h"

void kernel_sleep_ms(uint32_t ms) {
    uint32_t start = kernel_get_ticks_ms();

    while ((kernel_get_ticks_ms() - start) < ms) {
        __asm__ volatile ("hlt");
    }
}

void kernel_sleep_until_ms(uint32_t deadline_ms) {
    while ((int32_t)(deadline_ms - kernel_get_ticks_ms()) > 0) {
        __asm__ volatile ("hlt");
    }
}
