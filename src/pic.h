#ifndef PIC_H
#define PIC_H

void pic_remap();
void pic_send_EOI(unsigned char irq);

#endif