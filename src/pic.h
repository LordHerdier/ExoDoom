#pragma once
#include <stdint.h>

void pic_remap();
void pic_unmask_irq(unsigned char irq);
void pic_send_EOI(unsigned char irq);

/* SCRUM-227: each returns slave<<8 | master. */
uint16_t pic_read_irr(void);
uint16_t pic_read_isr(void);
uint16_t pic_read_imr(void);
uint16_t pic_read_elcr(void);

/* How many spurious IRQ7s (master) / IRQ15s (slave) have been absorbed. */
uint32_t pic_spurious_irq7_count(void);
uint32_t pic_spurious_irq15_count(void);

/* C halves of irq7_spurious_stub / irq15_spurious_stub (src/isr.s). */
void pic_irq7_spurious(void);
void pic_irq15_spurious(void);
