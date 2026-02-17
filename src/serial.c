#include "serial.h"
#include "io.h" 

void serial_init(void) {
    outb(0x3F8 + 1, 0x00);    // Disable all interrupts
    outb(0x3F8 + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(0x3F8 + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(0x3F8 + 1, 0x00);    //                  (hi byte)
    outb(0x3F8 + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(0x3F8 + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(0x3F8 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

static int serial_can_tx(void) {
    return inb(0x3F8 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_can_tx()) {}
    outb(0x3F8, (uint8_t)c);
}

void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}