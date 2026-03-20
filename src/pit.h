#ifndef PIT_H
#define PIT_H

#include <stdint.h>

void pitInit(uint32_t hz);
uint64_t timerTicks();
uint64_t timerMS();

#endif