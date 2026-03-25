#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "memory.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    memory_init();

    serial_print("Memory subsystem initialized\n");
    serial_print("Allocator base: ");
    serial_print_hex(memory_base_address());
    serial_print("\n");

    serial_flush();
    qemu_exit(0);
}
