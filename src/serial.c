#include "serial.h"
#include "io.h"

#define COM1 0x3F8

static int serial_tx_empty(void) {
    return inb(COM1 + 5) & 0x40;
}

void serial_flush(void) {
    while (!serial_tx_empty()) {}
}


void serial_init(void) {
    outb(COM1 + 1, 0x00);   // Disable all interrupts
    outb(COM1 + 3, 0x80);   // Enable DLAB (set baud rate divisor)
    outb(COM1 + 0, 0x03);   // Set divisor to 3 (lo byte) — 38400 baud
    outb(COM1 + 1, 0x00);   //                  (hi byte)
    outb(COM1 + 3, 0x03);   // 8 bits, no parity, one stop bit
    outb(COM1 + 2, 0xC7);   // Enable FIFO, clear them, 14-byte threshold
    outb(COM1 + 4, 0x0B);   // IRQs enabled, RTS/DSR set
}

void serial_print_hex(uint32_t num) {
    char hex[] = "0123456789ABCDEF";
    char buffer[9];
    buffer[8] = '\0';

    for (int i = 7; i >= 0; i--) {
        buffer[i] = hex[num & 0xF];
        num >>= 4;
    }

    serial_print(buffer);
}

void serial_print_hex64(uint64_t num) {
    char hex[] = "0123456789ABCDEF";
    char buffer[17];
    buffer[16] = '\0';

    for (int i = 15; i >= 0; i--) {
        buffer[i] = hex[num & 0xF];
        num >>= 4;
    }

    serial_print(buffer);
}

void serial_print_dec(uint32_t num) {
    char buf[16];
    int i = 0;

    if (num == 0) {
        serial_putc('0');
        return;
    }

    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }

    while (i > 0) {
        serial_putc(buf[--i]);
    }
}

static int serial_can_tx(void) {
    return inb(COM1 + 5) & 0x20;
}

void serial_putc(char c) {
    while (!serial_can_tx()) {}
    outb(COM1, (uint8_t)c);
}

void serial_print(const char* s) {
    while (*s) serial_putc(*s++);
}
