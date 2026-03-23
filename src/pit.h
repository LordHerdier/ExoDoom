#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pit_init(uint32_t hz);
uint64_t timer_ticks();
uint64_t timer_MS();

#endif