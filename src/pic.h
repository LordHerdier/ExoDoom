#pragma once
#include <stdint.h>

void pic_remap();
void pic_unmask_irq(unsigned char irq);
void pic_send_EOI(unsigned char irq);
