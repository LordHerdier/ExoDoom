#pragma once
#include <stdint.h>

void pit_init(uint32_t hz);
uint32_t kernel_get_ticks_ms();
uint8_t pit_take_print_pending();

