#pragma once
#include <stdint.h>

void pic_remap();
void pic_unmask_irq1(void);
void pic_send_EOI(unsigned char irq);
