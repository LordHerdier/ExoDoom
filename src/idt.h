#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idtInit();
void idtSetGate(int n, uint32_t handler);

#endif