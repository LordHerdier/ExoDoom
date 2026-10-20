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
 * insw/outsw — `count` 16-bit transfers between `port` and memory, as one
 * `rep insw`/`rep outsw` string instruction rather than a C loop over
 * inw()/outw(). Same bytes in the same order (x86 is little-endian, so the
 * low byte of each word lands first either way); the difference is cost
 * under virtualization. Each standalone IN/OUT is a VM exit to the device
 * model, while KVM batches a REP string I/O instruction into one exit per
 * buffer-sized chunk -- for ATA's 256-word sector that is ~1 exit instead
 * of 256. SCRUM-190's 28 MB WAD load is ~14M words, which as single inw()s
 * took ~3 minutes under -enable-kvm with interrupts off. Relies on DF
 * being clear, which the SysV ABI guarantees at every C call boundary.
 */
static inline void insw(uint16_t port, void *buf, uint32_t count) {
    __asm__ volatile ("rep insw"
                      : "+D"(buf), "+c"(count)
                      : "d"(port)
                      : "memory");
}

static inline void outsw(uint16_t port, const void *buf, uint32_t count) {
    __asm__ volatile ("rep outsw"
                      : "+S"(buf), "+c"(count)
                      : "d"(port)
                      : "memory");
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