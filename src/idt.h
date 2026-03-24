#pragma once
#include <stdint.h>

void idt_init();
void idt_set_gate(int n, uint32_t handler);

