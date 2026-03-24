#include <stdint.h>
#include "pit.h"
#include "sleep.h"

void kernel_sleep_ms(uint64_t ms) {
    uint64_t start = kernel_get_ticks_ms();

    while ((kernel_get_ticks_ms() - start) < ms){
        __asm__ volatile ("hlt");
    }
}
