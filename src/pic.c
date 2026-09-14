#include "pic.h"
#include "io.h"

#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_COMMAND PIC1
#define PIC1_DATA (PIC1+1)
#define PIC2_COMMAND PIC2
#define PIC2_DATA (PIC2+1)

// ICW - Initialization Command Word
// Think of this as "Hey PIC, here is how we want interrupts to work".

#define ICW1_INIT 0x10      // "Start init" - Without this, we would likely run into unpredictable situations where PIC stays as GRUB left it leading to critical overlap.
#define ICW1_ICW4 0x01      // Tell the PIC to expect ICW4 during initialization (required for 8086 mode).
#define ICW4_8086 0x01      // "Set operating mode" - puts PIC into 8086/88 mode - ensures interrupts behave correctly with CPU on x86

void pic_remap() {
    // Begin initialization
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4); io_wait();

    // Set vector offsets
    outb(PIC1_DATA, 0x20); io_wait();
    outb(PIC2_DATA, 0x28); io_wait();

    // Tell master about slave
    outb(PIC1_DATA, 4); io_wait();
    outb(PIC2_DATA, 2); io_wait();

    // Set 8086 mode
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Mask: unmask IRQ0 (timer) only. IRQ1 (keyboard) stays masked here --
    // pic_remap() now runs ahead of the TESTING branch (SCRUM-172), before
    // IRQ1's IDT vector is wired to irq1_stub, and unmasking it this early
    // would let a stray keyboard interrupt land on idt_init()'s default_stub,
    // which does a bare iretq with no EOI and would wedge IRQ1's in-service
    // bit at the PIC for good. pic_unmask_irq1() unmasks it once kbd_init()
    // has actually run -- see kernel_main and src/ps2.c's kbd_init().
    outb(PIC1_DATA, 0xFE); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
}

void pic_unmask_irq1(void) {
    uint8_t mask = inb(PIC1_DATA);
    outb(PIC1_DATA, mask & (uint8_t)~0x02);
}

void pic_send_EOI(unsigned char irq) {
    if (irq >= 8){
        outb(PIC2_COMMAND, 0x20);
    }

    outb(PIC1_COMMAND, 0x20);
}
