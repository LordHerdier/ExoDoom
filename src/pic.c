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
#define ICW1_ICW4 0x01      // "Where should interrupts go?" - Sets interrupt vector offsets - should help negate double faults.
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

    // Mask
    outb(PIC1_DATA, 0xFE); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
}

void pic_send_EOI(unsigned char irq) {
    if (irq >= 8){
        outb(PIC2_COMMAND, 0x20);
    }

    outb(PIC1_COMMAND, 0x20);
}
