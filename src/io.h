#pragma once
#include <stdint.h>

/*
 * io.h — Bare-metal x86 port I/O primitives.
 *
 * Extracted from serial.c so that PIC, PIT, and every other
 * hardware driver can share the same helpers.
 */

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * outw/inw — 16-bit port I/O (ATA's data register, 0x1F0, transfers a
 * whole sector as 256 words rather than 512 bytes).
 */
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * outl — 32-bit port write (used by qemu_exit via isa-debug-exit).
 */
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}

/*
 * inl — 32-bit port read (PCI configuration mechanism #1 reads its data
 *       register at 0xCFC as a whole dword; see src/pci.c).
 */
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/*
 * io_wait — Short delay for hardware that needs time between
 *           consecutive I/O operations (notably the 8259 PIC).
 *
 * Writing to port 0x80 (POST diagnostic port) is the traditional
 * ~1 µs delay trick.  Safe on QEMU and real hardware.
 */
static inline void io_wait(void) {
    outb(0x80, 0);
}