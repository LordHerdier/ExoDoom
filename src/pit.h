#pragma once
#include <stdint.h>

void pit_init(uint32_t hz);
uint64_t timer_ticks();
uint64_t timer_MS();

uint64_t kernel_get_ticks_ms();

