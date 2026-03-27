#include <stdint.h>
#include "multiboot.h"
#include "serial.h"
#include "memory.h"
#include "mmap.h"
#include "page_alloc.h"

static inline void qemu_exit(uint32_t code) {
    __asm__ volatile ("outl %0, %1" : : "a"(code), "Nd"(0xF4));
}

void kernel_main(uint32_t mb_info_addr) {
    serial_init();

    struct multiboot_info* mb = (struct multiboot_info*)mb_info_addr;

    serial_print("Kernel Booted\n");

    mmap_init(mb);

    memory_init();

    serial_print("Memory subsystem initialized\n");
    serial_print("Allocator base: ");
    serial_print_hex(memory_base_address());
    serial_print("\n");

    page_alloc_init(mb);

#ifdef DEBUG
    void* p1 = alloc_page();
    void* p2 = alloc_page();

    serial_print("Allocated page 1: ");
    serial_print_hex((uint32_t)p1);
    serial_print("\n");

    serial_print("Allocated page 2: ");
    serial_print_hex((uint32_t)p2);
    serial_print("\n");

    free_page(p1);
    serial_print("Freed page 1\n");

    free_page(p1);

    serial_print("Freed page 1\n");

    free_page(p1);
#endif 

    serial_flush();
    qemu_exit(0);
}
