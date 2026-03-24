#pragma once
#include <stdint.h>

void pic_remap();
void pic_send_EOI(unsigned char irq);
